#ifndef teca_mesh_h
#define teca_mesh_h

#include "teca_mesh_fwd.h"
#include "teca_dataset.h"
#include "teca_metadata.h"
#include "teca_array_collection.h"

/// class for geometric data
class teca_mesh : public teca_dataset
{
public:
    virtual ~teca_mesh() = default;

    // get metadata
    teca_metadata &get_metadata()
    { return m_impl->metadata; }

    const teca_metadata &get_metadata() const
    { return m_impl->metadata; }

    // set/get temporal metadata
    TECA_DATASET_METADATA(time, double, 1, m_impl->metadata)
    TECA_DATASET_METADATA(calendar, std::string, 1, m_impl->metadata)
    TECA_DATASET_METADATA(time_units, std::string, 1, m_impl->metadata)
    TECA_DATASET_METADATA(time_step, unsigned long, 1, m_impl->metadata)

    // get point centered data
    p_teca_array_collection get_point_arrays()
    { return m_impl->point_arrays; }

    const_p_teca_array_collection get_point_arrays() const
    { return m_impl->point_arrays; }

    // get cell centered data
    p_teca_array_collection get_cell_arrays()
    { return m_impl->cell_arrays; }

    const_p_teca_array_collection get_cell_arrays() const
    { return m_impl->cell_arrays; }

    // get edge centered data
    p_teca_array_collection get_edge_arrays()
    { return m_impl->edge_arrays; }

    const_p_teca_array_collection get_edge_arrays() const
    { return m_impl->edge_arrays; }

    // get face centered data
    p_teca_array_collection get_face_arrays()
    { return m_impl->face_arrays; }

    const_p_teca_array_collection get_face_arrays() const
    { return m_impl->face_arrays; }

    // get non-geometric data
    p_teca_array_collection get_information_arrays()
    { return m_impl->info_arrays; }

    const_p_teca_array_collection get_information_arrays() const
    { return m_impl->info_arrays; }

    // return true if the dataset is empty.
    virtual bool empty() const noexcept override;

    // copy metadata. always a deep copy.
    virtual void copy_metadata(const const_p_teca_dataset &) override;

    // copy data and metadata. shallow copy uses reference
    // counting, while copy duplicates the data.
    virtual void copy(const const_p_teca_dataset &) override;
    virtual void shallow_copy(const p_teca_dataset &) override;

    // swap internals of the two objects
    virtual void swap(p_teca_dataset &) override;

    // serialize the dataset to/from the given stream
    // for I/O or communication
    virtual void to_stream(teca_binary_stream &) const override;
    virtual void from_stream(teca_binary_stream &) override;

    // stream to/from human readable representation
    virtual void to_stream(std::ostream &) const override;
    virtual void from_stream(std::istream &) override {}

protected:
    teca_mesh();

public:
    struct impl_t
    {
        impl_t();
        //
        teca_metadata metadata;
        p_teca_array_collection point_arrays;
        p_teca_array_collection cell_arrays;
        p_teca_array_collection edge_arrays;
        p_teca_array_collection face_arrays;
        p_teca_array_collection info_arrays;
    };
    std::shared_ptr<impl_t> m_impl;
};

#endif
