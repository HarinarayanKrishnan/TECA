#include "teca_cf_reader.h"
#include "teca_file_util.h"
#include "teca_cartesian_mesh.h"
#include "teca_thread_pool.h"

#include <netcdf.h>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>
#include <utility>
#include <memory>

using std::endl;
using std::cerr;

#if defined(TECA_HAS_MPI)
#include <mpi.h>
#endif

#if defined(TECA_HAS_BOOST)
#include <boost/program_options.hpp>
#endif

// macro to help with netcdf data types
#define NC_DISPATCH_FP(tc_, code_)                          \
    switch (tc_)                                            \
    {                                                       \
    NC_DISPATCH_CASE(NC_FLOAT, float, code_)                \
    NC_DISPATCH_CASE(NC_DOUBLE, double, code_)              \
    default:                                                \
        TECA_ERROR("netcdf type code_ " << tc_              \
            << " is not a floating point type")             \
    }
#define NC_DISPATCH(tc_, code_)                             \
    switch (tc_)                                            \
    {                                                       \
    NC_DISPATCH_CASE(NC_BYTE, char, code_)                  \
    NC_DISPATCH_CASE(NC_UBYTE, unsigned char, code_)        \
    NC_DISPATCH_CASE(NC_CHAR, char, code_)                  \
    NC_DISPATCH_CASE(NC_SHORT, short int, code_)            \
    NC_DISPATCH_CASE(NC_USHORT, unsigned short int, code_)  \
    NC_DISPATCH_CASE(NC_INT, int, code_)                    \
    NC_DISPATCH_CASE(NC_UINT, unsigned int, code_)          \
    NC_DISPATCH_CASE(NC_INT64, long long, code_)            \
    NC_DISPATCH_CASE(NC_UINT64, unsigned long long, code_)  \
    NC_DISPATCH_CASE(NC_FLOAT, float, code_)                \
    NC_DISPATCH_CASE(NC_DOUBLE, double, code_)              \
    default:                                                \
        TECA_ERROR("netcdf type code_ " << tc_              \
            << " is not supported")                         \
    }
#define NC_DISPATCH_CASE(cc_, tt_, code_)   \
    case cc_:                               \
    {                                       \
        using NC_T = tt_;                   \
        code_                               \
        break;                              \
    }






// to deal with fortran fixed length strings
// which are not properly nulll terminated
static void crtrim(char *s, long n)
{
    if (!s || (n == 0)) return;
    char c = s[--n];
    while ((n > 0) && ((c == ' ') || (c == '\n') ||
        (c == '\t') || (c == '\r')))
    {
        s[n] = '\0';
        c = s[--n];
    }
}

// RAII for managing netcdf files
class netcdf_handle
{
public:
    // initialize with a handle returned from
    // nc_open/nc_create etc
    netcdf_handle(int h) : m_handle(h)
    {}

    // close the file during destruction
    ~netcdf_handle()
    { this->close(); }

    // this is a move only class, and should
    // only be initialized with an valid handle
    netcdf_handle() = delete;
    netcdf_handle(const netcdf_handle &) = delete;
    void operator=(const netcdf_handle &) = delete;

    // move construction takes ownership
    // from the other object
    netcdf_handle(netcdf_handle &&other)
    {
        m_handle = other.m_handle;
        other.m_handle = 0;
    }

    // move assignment takes ownership
    // from the other object
    void operator=(netcdf_handle &&other)
    {
        this->close();
        m_handle = other.m_handle;
        other.m_handle = 0;
    }

    // close the file
    void close()
    {
        if (m_handle)
        {
            nc_close(m_handle);
            m_handle = 0;
        }
    }

    // dereference a pointer to the object
    // returns a reference to the handle
    int &get()
    { return m_handle; }

private:
    int m_handle;
};

// data and task types
using read_variable_data_t = std::pair<unsigned long, p_teca_variant_array>;
using read_variable_task_t = std::packaged_task<read_variable_data_t()>;

using read_variable_queue_t =
    teca_thread_pool<read_variable_task_t, read_variable_data_t>;

using p_read_variable_queue_t = std::shared_ptr<read_variable_queue_t>;

// internals for the cf reader
class teca_cf_reader_internals
{
public:
    teca_cf_reader_internals()
    {}

    // helpers for dealing with cached file handles.
    // root rank opens all files during metadata parsing,
    // others ranks only open assigned files. in both
    // cases threads on each rank should share file
    // handles
    void close_handles();
    void clear_handles();
    void initialize_handles(const std::vector<std::string> &files);

    int get_handle(const std::string &path,
        const std::string &file, int &file_id,
        std::mutex *&file_mutex);

    int close_handle(const std::string &path);

public:
    using p_mutex_t = std::unique_ptr<std::mutex>;
    using handle_map_elem_t = std::pair<p_mutex_t, netcdf_handle*>;
    using handle_map_t = std::map<std::string, handle_map_elem_t>;

    teca_metadata metadata;
    std::mutex handle_mutex;
    handle_map_t handles;
};

// --------------------------------------------------------------------------
void teca_cf_reader_internals::close_handles()
{
    handle_map_t::iterator it = this->handles.begin();
    handle_map_t::iterator last = this->handles.end();
    for (; it != last; ++it)
    {
        delete it->second.second;
        it->second.second = nullptr;
    }
}

// --------------------------------------------------------------------------
void teca_cf_reader_internals::clear_handles()
{
    handle_map_t::iterator it = this->handles.begin();
    handle_map_t::iterator last = this->handles.end();
    for (; it != last; ++it)
    {
        delete it->second.second;
        it->second.second = nullptr;
        it->second.first = nullptr;
    }
    this->handles.clear();
}

// --------------------------------------------------------------------------
void teca_cf_reader_internals::initialize_handles(
    const std::vector<std::string> &files)
{
    this->clear_handles();
    size_t n_files = files.size();
    for (size_t i = 0; i < n_files; ++i)
        this->handles[files[i]] = std::make_pair(
            std::unique_ptr<std::mutex>(new std::mutex), nullptr);
}

// --------------------------------------------------------------------------
int teca_cf_reader_internals::close_handle(const std::string &file)
{
    // lock the mutex
    std::lock_guard<std::mutex> lock(this->handle_mutex);

    // get the current handle
    handle_map_t::iterator it = this->handles.find(file);

#if defined(TECA_DEBUG)
    if (it == this->handles.end())
    {
        // map should already be initialized. if this occurs
        // there's a bug in the reader class
        TECA_ERROR("File \"" << file << "\" is not in the handle cache")
        return -1;
    }
#endif

    delete it->second.second;
    it->second.second = nullptr;
    return 0;
}

// --------------------------------------------------------------------------
int teca_cf_reader_internals::get_handle(const std::string &path,
    const std::string &file, int &file_id, std::mutex *&file_mutex)
{
    // lock the mutex
    std::lock_guard<std::mutex> lock(this->handle_mutex);

    // get the current handle
    handle_map_t::iterator it = this->handles.find(file);

#if defined(TECA_DEBUG)
    if (it == this->handles.end())
    {
        // map should already be initialized. if this occurs
        // there's a bug in the reader class
        TECA_ERROR("File \"" << file << "\" is not in the handle cache")
        return -1;
    }
#endif

    // return the cached value
    file_mutex = it->second.first.get();
    if (it->second.second)
    {
        file_id = it->second.second->get();
        return 0;
    }

    // open the file
    std::string file_path = path + PATH_SEP + file;
    int ierr = 0;
    if ((ierr = nc_open(file_path.c_str(), NC_NOWRITE, &file_id)) != NC_NOERR)
    {
        TECA_ERROR("Failed to open " << file << ". " << nc_strerror(ierr))
        return -1;
    }

    // cache the handle
    it->second.second = new netcdf_handle(file_id);

    return 0;
}

// function that reads and returns a variable from the
// named file. we're doing this so we can do thread
// parallel I/O to hide some of the cost of opening files
// on Lustre and to hide the cost of reading time coordinate
// which is typically very expensive as NetCDF stores
// unlimted dimensions non-contiguously
class read_variable
{
public:
    read_variable(p_teca_cf_reader_internals reader_internals,
        const std::string &path, const std::string &file, unsigned long id,
        const std::string &variable) : m_reader_internals(reader_internals),
        m_path(path), m_file(file), m_variable(variable), m_id(id)
    {}

    std::pair<unsigned long, p_teca_variant_array> operator()()
    {
        p_teca_variant_array var;

        // get a handle to the file. managed by the reader
        // since it will reuse the handle when it needs to read
        // mesh based data
        int ierr = 0;
        int file_id = 0;
        std::mutex *file_mutex = nullptr;
        if (m_reader_internals->get_handle(m_path, m_file, file_id, file_mutex))
        {
            TECA_ERROR("Failed to get handle to read variable \"" << m_variable
                << "\" from \"" << m_file << "\"")
            return std::make_pair(m_id, nullptr);
        }

        // query variable attributes
        int var_id = 0;
        size_t var_size = 0;
        nc_type var_type = 0;

        if (((ierr = nc_inq_dimid(file_id, m_variable.c_str(), &var_id)) != NC_NOERR)
            || ((ierr = nc_inq_dimlen(file_id, var_id, &var_size)) != NC_NOERR)
            || ((ierr = nc_inq_varid(file_id, m_variable.c_str(), &var_id)) != NC_NOERR)
            || ((ierr = nc_inq_vartype(file_id, var_id, &var_type)) != NC_NOERR))
        {
            m_reader_internals->close_handle(m_file);
            TECA_ERROR("Failed to read metadata for variable \"" << m_variable
                << "\" from \"" << m_file << "\". " << nc_strerror(ierr))
            return std::make_pair(m_id, nullptr);
        }

        // allocate a buffer and read the variable.
        NC_DISPATCH_FP(var_type,
            size_t start = 0;
            p_teca_variant_array_impl<NC_T> var = teca_variant_array_impl<NC_T>::New();
            var->resize(var_size);
            if ((ierr = nc_get_vara(file_id, var_id, &start, &var_size, var->get())) != NC_NOERR)
            {
                m_reader_internals->close_handle(m_file);
                TECA_ERROR("Failed to read variable \"" << m_variable  << "\" from \""
                    << m_file << "\". " << nc_strerror(ierr))
                return std::make_pair(m_id, nullptr);
            }
            // success!
            m_reader_internals->close_handle(m_file);
            return std::make_pair(m_id, var);
            )

        // unsupported type
        m_reader_internals->close_handle(m_file);
        TECA_ERROR("Failed to read variable \"" << m_variable
            << "\" from \"" << m_file << "\". Unsupported data type")
        return std::make_pair(m_id, nullptr);
    }

private:
    p_teca_cf_reader_internals m_reader_internals;
    std::string m_path;
    std::string m_file;
    std::string m_variable;
    unsigned long m_id;
};





// --------------------------------------------------------------------------
teca_cf_reader::teca_cf_reader() :
    files_regex(""),
    x_axis_variable("lon"),
    y_axis_variable("lat"),
    z_axis_variable(""),
    t_axis_variable("time"),
    thread_pool_size(-1),
    internals(new teca_cf_reader_internals)
{}

// --------------------------------------------------------------------------
teca_cf_reader::~teca_cf_reader()
{
    this->internals->clear_handles();
}

#if defined(TECA_HAS_BOOST)
// --------------------------------------------------------------------------
void teca_cf_reader::get_properties_description(
    const std::string &prefix, options_description &global_opts)
{
    options_description opts("Options for "
        + (prefix.empty()?"teca_cf_reader":prefix));

    opts.add_options()
        TECA_POPTS_GET(std::string, prefix, files_regex,
            "a regular expression that matches the set of files "
            "comprising the dataset")
        TECA_POPTS_GET(std::string, prefix, file_name,
            "a single path/file name to read. may be used in place "
            "of files_regex")
        TECA_POPTS_GET(std::string, prefix, x_axis_variable,
            "name of variable that has x axis coordinates (lon)")
        TECA_POPTS_GET(std::string, prefix, y_axis_variable,
            "name of variable that has y axis coordinates (lat)")
        TECA_POPTS_GET(std::string, prefix, z_axis_variable,
            "name of variable that has z axis coordinates ()")
        TECA_POPTS_GET(std::string, prefix, t_axis_variable,
            "name of variable that has t axis coordinates (time)")
        TECA_POPTS_GET(int, prefix, thread_pool_size,
            "set the number of I/O threads (-1)")
        ;

    global_opts.add(opts);
}

// --------------------------------------------------------------------------
void teca_cf_reader::set_properties(const std::string &prefix,
    variables_map &opts)
{
    TECA_POPTS_SET(opts, std::string, prefix, files_regex)
    TECA_POPTS_SET(opts, std::string, prefix, file_name)
    TECA_POPTS_SET(opts, std::string, prefix, x_axis_variable)
    TECA_POPTS_SET(opts, std::string, prefix, y_axis_variable)
    TECA_POPTS_SET(opts, std::string, prefix, z_axis_variable)
    TECA_POPTS_SET(opts, std::string, prefix, t_axis_variable)
    TECA_POPTS_SET(opts, int, prefix, thread_pool_size)
}
#endif

// --------------------------------------------------------------------------
void teca_cf_reader::set_modified()
{
    // clear cached metadata before forwarding on to
    // the base class.
    this->clear_cached_metadata();
    teca_algorithm::set_modified();
}

// --------------------------------------------------------------------------
void teca_cf_reader::clear_cached_metadata()
{
    this->internals->metadata.clear();
    this->internals->clear_handles();
}

// --------------------------------------------------------------------------
teca_metadata teca_cf_reader::get_output_metadata(
    unsigned int port,
    const std::vector<teca_metadata> &input_md)
{
#ifdef TECA_DEBUG
    cerr << teca_parallel_id()
        << "teca_cf_reader::get_output_metadata" << endl;
#endif
    (void)port;
    (void)input_md;

    // return cached metadata. cache is cleared if
    // any of the algorithms properties are modified
    if (this->internals->metadata)
        return this->internals->metadata;

    // TODO -- use teca specific communicator
    int rank = 0;
    int n_ranks = 1;
#if defined(TECA_HAS_MPI)
    int is_init = 0;
    MPI_Initialized(&is_init);
    if (is_init)
    {
        MPI_Comm_rank(MPI_COMM_WORLD, &rank);
        MPI_Comm_size(MPI_COMM_WORLD, &n_ranks);
    }

    teca_binary_stream bstr;
    unsigned long bstr_size;
#endif

    // only rank 0 will parse the dataset. once
    // parsed metadata is broadcast to all
    int root_rank = n_ranks - 1;
    if (rank == root_rank)
    {
        std::vector<std::string> files;
        std::string path;

        if (!this->file_name.empty())
        {
            // use file name
            path = teca_file_util::path(this->file_name);
            files.push_back(teca_file_util::filename(this->file_name));
        }
        else
        {
            // use regex
            std::string regex = teca_file_util::filename(this->files_regex);
            path = teca_file_util::path(this->files_regex);

            if (teca_file_util::locate_files(path, regex, files))
            {
                TECA_ERROR(
                    << "Failed to locate any files" << endl
                    << this->files_regex << endl
                    << path << endl
                    << regex)
                return teca_metadata();
            }
        }

        int ierr = 0;
        int file_id = 0;
        std::string file = path + PATH_SEP + files[0];

        // get mesh coordinates and dimensions
        int x_id = 0;
        int y_id = 0;
        int z_id = 0;
        size_t n_x = 1;
        size_t n_y = 1;
        size_t n_z = 1;
        nc_type x_t = 0;
        nc_type y_t = 0;
        nc_type z_t = 0;
        int n_vars = 0;

        if ((ierr = nc_open(file.c_str(), NC_NOWRITE, &file_id)) != NC_NOERR)
        {
            TECA_ERROR("Failed to open " << file << endl << nc_strerror(ierr))
            return teca_metadata();
        }

        // initialize the file map
        this->internals->initialize_handles(files);

        // cache the file handle
        this->internals->handles[files[0]] = std::make_pair(
            std::unique_ptr<std::mutex>(new std::mutex), nullptr);

        // query mesh axes
        if (((ierr = nc_inq_dimid(file_id, x_axis_variable.c_str(), &x_id)) != NC_NOERR)
            || ((ierr = nc_inq_dimlen(file_id, x_id, &n_x)) != NC_NOERR)
            || ((ierr = nc_inq_varid(file_id, x_axis_variable.c_str(), &x_id)) != NC_NOERR)
            || ((ierr = nc_inq_vartype(file_id, x_id, &x_t)) != NC_NOERR))
        {
            this->clear_cached_metadata();
            TECA_ERROR(
                << "Failed to query x axis variable \"" << x_axis_variable
                << "\" in file \"" << file << "\"" << endl
                << nc_strerror(ierr))
            return teca_metadata();
        }

        if (!y_axis_variable.empty()
            && (((ierr = nc_inq_dimid(file_id, y_axis_variable.c_str(), &y_id)) != NC_NOERR)
            || ((ierr = nc_inq_dimlen(file_id, y_id, &n_y)) != NC_NOERR)
            || ((ierr = nc_inq_varid(file_id, y_axis_variable.c_str(), &y_id)) != NC_NOERR)
            || ((ierr = nc_inq_vartype(file_id, y_id, &y_t)) != NC_NOERR)))
        {
            this->clear_cached_metadata();
            TECA_ERROR(
                << "Failed to query y axis variable \"" << y_axis_variable
                << "\" in file \"" << file << "\"" << endl
                << nc_strerror(ierr))
            return teca_metadata();
        }

        if (!z_axis_variable.empty()
            && (((ierr = nc_inq_dimid(file_id, z_axis_variable.c_str(), &z_id)) != NC_NOERR)
            || ((ierr = nc_inq_dimlen(file_id, z_id, &n_z)) != NC_NOERR)
            || ((ierr = nc_inq_varid(file_id, z_axis_variable.c_str(), &z_id)) != NC_NOERR)
            || ((ierr = nc_inq_vartype(file_id, z_id, &z_t)) != NC_NOERR)))
        {
            this->clear_cached_metadata();
            TECA_ERROR(
                << "Failed to query z axis variable \"" << z_axis_variable
                << "\" in file \"" << file << "\"" << endl
                << nc_strerror(ierr))
            return teca_metadata();
        }

        // enumerate mesh arrays and their attributes
        if (((ierr = nc_inq_nvars(file_id, &n_vars)) != NC_NOERR))
        {
            this->clear_cached_metadata();
            TECA_ERROR(
                << "Failed to get the number of variables in file \""
                << file << "\"" << endl
                << nc_strerror(ierr))
            return teca_metadata();
        }

        teca_metadata atrs;
        std::vector<std::string> vars;
        std::vector<std::string> time_vars; // anything that has the time dimension as it's only dim
        for (int i = 0; i < n_vars; ++i)
        {
            char var_name[NC_MAX_NAME + 1] = {'\0'};
            nc_type var_type = 0;
            int n_dims = 0;
            int dim_id[NC_MAX_VAR_DIMS] = {0};
            int n_atts = 0;

            if ((ierr = nc_inq_var(file_id, i, var_name,
                    &var_type, &n_dims, dim_id, &n_atts)) != NC_NOERR)
            {
                this->clear_cached_metadata();
                TECA_ERROR(
                    << "Failed to query " << i << "th variable, "
                    << file << endl << nc_strerror(ierr))
                return teca_metadata();
            }

            // skip scalars
            if (n_dims == 0)
                continue;

            std::vector<size_t> dims;
            std::vector<std::string> dim_names;
            for (int ii = 0; ii < n_dims; ++ii)
            {
                char dim_name[NC_MAX_NAME + 1] = {'\0'};
                size_t dim = 0;
                if ((ierr = nc_inq_dim(file_id, dim_id[ii], dim_name, &dim)) != NC_NOERR)
                {
                    this->clear_cached_metadata();
                    TECA_ERROR(
                        << "Failed to query " << ii << "th dimension of variable, "
                        << var_name << ", " << file << endl << nc_strerror(ierr))
                    return teca_metadata();
                }

                dim_names.push_back(dim_name);
                dims.push_back(dim);
            }

            vars.push_back(var_name);

            if ((n_dims == 1) && (dim_names[0] == t_axis_variable))
                time_vars.push_back(var_name);

            teca_metadata atts;
            atts.insert("id", i);
            atts.insert("dims", dims);
            atts.insert("dim_names", dim_names);
            atts.insert("type", var_type);
            atts.insert("centering", std::string("point"));

            char *buffer = nullptr;
            for (int ii = 0; ii < n_atts; ++ii)
            {
                char att_name[NC_MAX_NAME + 1] = {'\0'};
                nc_type att_type = 0;
                size_t att_len = 0;
                if (((ierr = nc_inq_attname(file_id, i, ii, att_name)) != NC_NOERR)
                    || ((ierr = nc_inq_att(file_id, i, att_name, &att_type, &att_len)) != NC_NOERR))
                {
                    this->clear_cached_metadata();
                    TECA_ERROR(
                        << "Failed to query " << ii << "th attribute of variable, "
                        << var_name << ", " << file << endl << nc_strerror(ierr))
                    return teca_metadata();
                }
                if (att_type == NC_CHAR)
                {
                    buffer = static_cast<char*>(realloc(buffer, att_len + 1));
                    buffer[att_len] = '\0';
                    nc_get_att_text(file_id, i, att_name, buffer);
                    crtrim(buffer, att_len);
                    atts.insert(att_name, std::string(buffer));
                }
            }
            free(buffer);

            atrs.insert(var_name, atts);
        }

        this->internals->metadata.insert("variables", vars);
        this->internals->metadata.insert("attributes", atrs);
        this->internals->metadata.insert("time variables", time_vars);

        // read spatial coordinate arrays
        p_teca_variant_array x_axis;
        NC_DISPATCH_FP(x_t,
            size_t x_0 = 0;
            p_teca_variant_array_impl<NC_T> x = teca_variant_array_impl<NC_T>::New(n_x);
            if ((ierr = nc_get_vara(file_id, x_id, &x_0, &n_x, x->get())) != NC_NOERR)
            {
                this->clear_cached_metadata();
                TECA_ERROR(
                    << "Failed to read x axis, " << x_axis_variable << endl
                    << file << endl << nc_strerror(ierr))
                return teca_metadata();
            }
            x_axis = x;
            )

        p_teca_variant_array y_axis;
        if (!y_axis_variable.empty())
        {
            NC_DISPATCH_FP(y_t,
                size_t y_0 = 0;
                p_teca_variant_array_impl<NC_T> y = teca_variant_array_impl<NC_T>::New(n_y);
                if ((ierr = nc_get_vara(file_id, y_id, &y_0, &n_y, y->get())) != NC_NOERR)
                {
                    this->clear_cached_metadata();
                    TECA_ERROR(
                        << "Failed to read y axis, " << y_axis_variable << endl
                        << file << endl << nc_strerror(ierr))
                    return teca_metadata();
                }
                y_axis = y;
                )
        }
        else
        {
            NC_DISPATCH_FP(x_t,
                p_teca_variant_array_impl<NC_T> y = teca_variant_array_impl<NC_T>::New(1);
                y->set(0, NC_T());
                y_axis = y;
                )
        }

        p_teca_variant_array z_axis;
        if (!z_axis_variable.empty())
        {
            NC_DISPATCH_FP(z_t,
                size_t z_0 = 0;
                p_teca_variant_array_impl<NC_T> z = teca_variant_array_impl<NC_T>::New(n_z);
                if ((ierr = nc_get_vara(file_id, z_id, &z_0, &n_z, z->get())) != NC_NOERR)
                {
                    this->clear_cached_metadata();
                    TECA_ERROR(
                        << "Failed to read z axis, " << z_axis_variable << endl
                        << file << endl << nc_strerror(ierr))
                    return teca_metadata();
                }
                z_axis = z;
                )
        }
        else
        {
            NC_DISPATCH_FP(x_t,
                p_teca_variant_array_impl<NC_T> z = teca_variant_array_impl<NC_T>::New(1);
                z->set(0, NC_T());
                z_axis = z;
                )
        }

        // collect time steps from this and the rest of the files.
        // there are a couple of  performance issues on Lustre.
        // 1) opening a file is slow, there's latency due to contentions
        // 2) reading the time axis is very slow as it's not stored
        //    contiguously by convention. ie. time is an "unlimted"
        //    NetCDF dimension.
        // when procesing large numbers of files these issues kill
        // serial performance. hence we are reading time dimension
        // in parallel.
        read_variable_queue_t thread_pool(this->thread_pool_size, true);
        std::vector<unsigned long> step_count;
        p_teca_variant_array t_axis;
        if (!t_axis_variable.empty())
        {
            // assign the reads to threads
            size_t n_files = files.size();
            for (size_t i = 0; i < n_files; ++i)
            {
                read_variable reader(this->internals, path,
                    files[i], i, this->t_axis_variable);
                read_variable_task_t task(reader);
                thread_pool.push_task(task);
            }

            // wait for the results
            std::vector<read_variable_data_t> tmp;
            tmp.reserve(n_files);
            thread_pool.wait_data(tmp);

            // unpack the results. map is used to ensure the correct
            // file to time association.
            std::map<unsigned long, p_teca_variant_array>
                time_arrays(tmp.begin(), tmp.end());
            t_axis = time_arrays[0];
            if (!t_axis)
            {
                TECA_ERROR("Failed to read time axis")
                return teca_metadata();
            }
            step_count.push_back(time_arrays[0]->size());

            for (size_t i = 1; i < n_files; ++i)
            {
                p_teca_variant_array tmp = time_arrays[i];
                t_axis->append(*tmp.get());
                step_count.push_back(tmp->size());
            }
        }
        else
        {
            step_count.push_back(1);

            NC_DISPATCH_FP(x_t,
                p_teca_variant_array_impl<NC_T> t = teca_variant_array_impl<NC_T>::New(1);
                t->set(0, NC_T());
                t_axis = t;
                )
        }

        teca_metadata coords;
        coords.insert("x_variable", x_axis_variable);
        coords.insert("y_variable", (z_axis_variable.empty() ? "y" : z_axis_variable));
        coords.insert("z_variable", (z_axis_variable.empty() ? "z" : z_axis_variable));
        coords.insert("t_variable", (z_axis_variable.empty() ? "t" : z_axis_variable));
        coords.insert("x", x_axis);
        coords.insert("y", y_axis);
        coords.insert("z", z_axis);
        coords.insert("t", t_axis);

        std::vector<size_t> whole_extent(6, 0);
        whole_extent[1] = n_x - 1;
        whole_extent[3] = n_y - 1;
        whole_extent[5] = n_z - 1;
        this->internals->metadata.insert("whole_extent", whole_extent);
        this->internals->metadata.insert("coordinates", coords);
        this->internals->metadata.insert("files", files);
        this->internals->metadata.insert("root", path);
        this->internals->metadata.insert("step_count", step_count);
        this->internals->metadata.insert("number_of_time_steps", t_axis->size());

#if defined(TECA_HAS_MPI)
        if (is_init)
        {
            // broadcast the metadata to other ranks
            this->internals->metadata.to_stream(bstr);
            bstr_size = bstr.size();

            // TODO -- use teca specific communicator
            if (MPI_Bcast(&bstr_size, 1, MPI_UNSIGNED_LONG,
                root_rank, MPI_COMM_WORLD) ||
                MPI_Bcast(bstr.get_data(), bstr_size,
                    MPI_BYTE, root_rank, MPI_COMM_WORLD))
            {
                this->clear_cached_metadata();
                TECA_ERROR("Failed to broadcast internals")
                return teca_metadata();
            }
        }
#endif
    }
#if defined(TECA_HAS_MPI)
    else
    if (is_init)
    {
        // all other ranks receive the metadata from the root
        if (MPI_Bcast(&bstr_size, 1, MPI_UNSIGNED_LONG,
            root_rank, MPI_COMM_WORLD))
        {
            this->clear_cached_metadata();
            TECA_ERROR("Failed to broadcast metadata")
            return teca_metadata();
        }

        bstr.resize(bstr_size);

        if (MPI_Bcast(bstr.get_data(), bstr_size,
                 MPI_BYTE, root_rank, MPI_COMM_WORLD))
        {
            this->clear_cached_metadata();
            TECA_ERROR("Failed to broadcast metadata")
            return teca_metadata();
        }

        bstr.rewind();
        this->internals->metadata.from_stream(bstr);

        // initialize the file map
        std::vector<std::string> files;
        this->internals->metadata.get("files", files);
        this->internals->initialize_handles(files);
    }
#endif

    return this->internals->metadata;
}



    // write the output
    int ierr = 0;
    int ncid = -1;
    if ((ierr = nc_create(out_file.c_str(), NC_CLOBBER, &ncid)))
    {
        cerr << "error opening \"" << out_file << "\"" << endl
            << nc_strerror(ierr) << endl;
        return -1;
    }
    int lat_did = -1;
    ierr = nc_def_dim(ncid, "lat", n_lat, &lat_did);
    int lat_vid = -1;
    ierr = nc_def_var(ncid, "lat", NC_FLOAT, 1, &lat_did, &lat_vid);
    ierr = nc_put_att_text(ncid, lat_vid, "units", 13, "degrees_north");

    int lon_did = -1;
    ierr = nc_def_dim(ncid, "lon", n_lon, &lon_did);
    int lon_vid = -1;
    ierr = nc_def_var(ncid, "lon", NC_FLOAT, 1, &lon_did, &lon_vid);

    int mask_vid = -1;
    int dim_ids[2] = {lat_did, lon_did};
    ierr = nc_def_var(ncid, "mask", NC_FLOAT, 2, dim_ids, &mask_vid);
    ierr = nc_put_att_text(ncid, lon_vid, "units", 13, "degrees_east");

    ierr = nc_enddef(ncid);

    vector<float> lat(n_lat);
    float dlat = 180.0f/(n_lat - 1.0f);
    for (long i = 0; i < n_lat; ++i)
        lat[i] = -90.0f + i*dlat;
    ierr = nc_put_var_float(ncid, lat_vid, lat.data());

    vector<float> lon(n_lon);
    float dlon = 360.0f/(n_lon - 1.0f);
    for (long i = 0; i < n_lon; ++i)
        lon[i] = i*dlon;
    ierr = nc_put_var_float(ncid, lon_vid, lon.data());

	ifstream infi(in_file.c_str());
	if (!infi.good())
    {
        cerr << "error opening " << in_file << endl;
        return -1;
    }
    std::vector<float> mask;
    mask.resize(n_pts);
    for (long i = 0; i < n_pts; ++i)
    {
        if (!infi.good())
        {
            cerr << "Error reading mask" << endl;
            return -1;
        }
        infi >> mask[i];
    }
    infi.close();
    ierr = nc_put_var_float(ncid, mask_vid, mask.data());

    ierr = nc_close(ncid);

// --------------------------------------------------------------------------
const_p_teca_dataset teca_cf_reader::execute(
    unsigned int,
    const std::vector<const_p_teca_dataset> &input_data,
    const teca_metadata &request)
{
#ifdef TECA_DEBUG
    cerr << teca_parallel_id()
        << "teca_cf_reader::execute" << endl;
#endif
    (void)input_data;

    // get coordinates
    teca_metadata coords;
    if (this->internals->metadata.get("coordinates", coords))
    {
        TECA_ERROR("metadata is missing \"coordinates\"")
        return nullptr;
    }

    p_teca_variant_array in_x, in_y, in_z, in_t;
    if (!(in_x = coords.get("x")) || !(in_y = coords.get("y"))
        || !(in_z = coords.get("z")) || !(in_t = coords.get("t")))
    {
        TECA_ERROR("metadata is missing coordinate arrays")
        return nullptr;
    }

    // get request
    unsigned long time_step = 0;
    request.get("time_step", time_step);


    unsigned long whole_extent[6] = {0};
    if (this->internals->metadata.get("whole_extent", whole_extent, 6))
    {
        TECA_ERROR("time_step=" << time_step
            << " metadata is missing \"whole_extent\"")
        return nullptr;
    }

    unsigned long extent[6] = {0};
    if (request.get("extent", extent, 6))
        memcpy(extent, whole_extent, 6*sizeof(unsigned long));

    // requesting arrays is optional, but it's an error
    // to request an array that isn't present.
    std::vector<std::string> arrays;
    request.get("arrays", arrays);

    // slice axes on the requested extent
    p_teca_variant_array out_x = in_x->new_copy(extent[0], extent[1]);
    p_teca_variant_array out_y = in_y->new_copy(extent[2], extent[3]);
    p_teca_variant_array out_z = in_z->new_copy(extent[4], extent[5]);

    // TODO -- requesting out of bounds time step should be an error
    // need to ignore for now because we don't have a temporal regrid
    double t = 0.0;
    if (in_t && (time_step < in_t->size()))
        in_t->get(time_step, t);

    // locate file with this time step
    std::vector<unsigned long> step_count;
    if (this->internals->metadata.get("step_count", step_count))
    {
        TECA_ERROR("time_step=" << time_step
            << " metadata is missing \"step_count\"")
        return nullptr;
    }

    unsigned long idx = 0;
    unsigned long count = 0;
    for (unsigned int i = 1;
        (i < step_count.size()) && ((count + step_count[i-1]) <= time_step);
        ++idx, ++i)
    {
        count += step_count[i-1];
    }
    unsigned long offs = time_step - count;

    std::string path;
    std::string file;
    if (this->internals->metadata.get("root", path)
        || this->internals->metadata.get("files", idx, file))
    {
        TECA_ERROR("time_step=" << time_step
            << " Failed to locate file for time step " << time_step)
        return nullptr;
    }

    // get the file handle for this step
    int ierr = 0;
    int file_id = 0;
    std::mutex *file_mutex = nullptr;
    if (this->internals->get_handle(path, file, file_id, file_mutex))
    {
        TECA_ERROR("time_step=" << time_step << " Failed to get handle")
        return nullptr;
    }

    // create output dataset
    p_teca_cartesian_mesh mesh = teca_cartesian_mesh::New();
    mesh->set_x_coordinates(out_x);
    mesh->set_y_coordinates(out_y);
    mesh->set_z_coordinates(out_z);
    mesh->set_time(t);
    mesh->set_time_step(time_step);
    mesh->set_whole_extent(whole_extent);
    mesh->set_extent(extent);

    // get the time offset
    teca_metadata atrs;
    if (this->internals->metadata.get("attributes", atrs))
    {
        TECA_ERROR("time_step=" << time_step
            << " metadata missing \"attributes\"")
        return nullptr;
    }

    teca_metadata time_atts;
    std::string calendar;
    std::string units;
    if (!atrs.get("time", time_atts)
       && !time_atts.get("calendar", calendar)
       && !time_atts.get("units", units))
    {
        mesh->set_calendar(calendar);
        mesh->set_time_units(units);
    }

    // figure out the mapping between our extent and netcdf
    // representation
    std::vector<std::string> mesh_dim_names;
    std::vector<size_t> starts;
    std::vector<size_t> counts;
    size_t mesh_size = 1;
    if (!t_axis_variable.empty())
    {
        mesh_dim_names.push_back(t_axis_variable);
        starts.push_back(offs);
        counts.push_back(1);
    }
    if (!z_axis_variable.empty())
    {
        mesh_dim_names.push_back(z_axis_variable);
        starts.push_back(extent[4]);
        size_t count = extent[5] - extent[4] + 1;
        counts.push_back(count);
        mesh_size *= count;
    }
    if (!y_axis_variable.empty())
    {
        mesh_dim_names.push_back(y_axis_variable);
        starts.push_back(extent[2]);
        size_t count = extent[3] - extent[2] + 1;
        counts.push_back(count);
        mesh_size *= count;
    }
    if (!x_axis_variable.empty())
    {
        mesh_dim_names.push_back(x_axis_variable);
        starts.push_back(extent[0]);
        size_t count = extent[1] - extent[0] + 1;
        counts.push_back(count);
        mesh_size *= count;
    }

    // read requested arrays
    size_t n_arrays = arrays.size();
    for (size_t i = 0; i < n_arrays; ++i)
    {
        // get metadata
        teca_metadata atts;
        int type = 0;
        int id = 0;
        p_teca_string_array dim_names;

        if (atrs.get(arrays[i], atts)
            || atts.get("type", 0, type)
            || atts.get("id", 0, id)
            || !(dim_names = std::dynamic_pointer_cast<teca_string_array>(atts.get("dim_names"))))
        {
            TECA_ERROR("metadata issue can't read \"" << arrays[i] << "\"")
            continue;
        }

        // check if it's a mesh variable
        bool mesh_var = false;
        size_t n_dims = dim_names->size();
        if (n_dims == mesh_dim_names.size())
        {
            mesh_var = true;
            for (unsigned int ii = 0; ii < n_dims; ++ii)
            {
                if (dim_names->get(ii) != mesh_dim_names[ii])
                {
                    mesh_var = false;
                    break;
                }
            }
        }
        if (!mesh_var)
        {
            TECA_ERROR("time_step=" << time_step
                << " dimension mismatch. \"" << arrays[i]
                << "\" is not a mesh variable")
            continue;
        }

        // read
        p_teca_variant_array array;
        NC_DISPATCH(type,
            std::lock_guard<std::mutex> lock(*file_mutex);
            p_teca_variant_array_impl<NC_T> a = teca_variant_array_impl<NC_T>::New(mesh_size);
            if ((ierr = nc_get_vara(file_id,  id, &starts[0], &counts[0], a->get())) != NC_NOERR)
            {
                TECA_ERROR("time_step=" << time_step
                    << " Failed to read variable \"" << arrays[i] << "\" "
                    << file << endl << nc_strerror(ierr))
                continue;
            }
            array = a;
            )
        mesh->get_point_arrays()->append(arrays[i], array);
    }

    // read time vars
    std::vector<std::string> time_vars;
    this->internals->metadata.get("time variables", time_vars);
    size_t n_time_vars = time_vars.size();
    for (size_t i = 0; i < n_time_vars; ++i)
    {
        // get metadata
        teca_metadata atts;
        int type = 0;
        int id = 0;

        if (atrs.get(time_vars[i], atts)
            || atts.get("type", 0, type)
            || atts.get("id", 0, id))
        {
            TECA_ERROR("time_step=" << time_step
                << " metadata issue can't read \"" << time_vars[i] << "\"")
            continue;
        }

        // read
        int ierr = 0;
        p_teca_variant_array array;
        size_t one = 1;
        NC_DISPATCH(type,
            std::lock_guard<std::mutex> lock(*file_mutex);
            p_teca_variant_array_impl<NC_T> a = teca_variant_array_impl<NC_T>::New(1);
            if ((ierr = nc_get_vara(file_id,  id, &starts[0], &one, a->get())) != NC_NOERR)
            {
                TECA_ERROR("time_step=" << time_step
                    << " Failed to read \"" << time_vars[i] << "\" "
                    << file << endl << nc_strerror(ierr))
                continue;
            }
            array = a;
            )
        mesh->get_information_arrays()->append(time_vars[i], array);
    }

    return mesh;
}
