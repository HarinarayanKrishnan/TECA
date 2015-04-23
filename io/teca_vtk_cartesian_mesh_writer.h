#ifndef teca_vtk_cartesian_mesh_writer_h
#define teca_vtk_cartesian_mesh_writer_h

#include "teca_algorithm.h"
#include "teca_metadata.h"

#include <memory>
#include <vector>
#include <string>

class teca_vtk_cartesian_mesh_writer;

using p_teca_vtk_cartesian_mesh_writer
    = std::shared_ptr<teca_vtk_cartesian_mesh_writer>;

using const_p_teca_vtk_cartesian_mesh_writer
    = std::shared_ptr<const teca_vtk_cartesian_mesh_writer>;

/**
an algorithm that writes cartesian meshes in VTK format.
*/
class teca_vtk_cartesian_mesh_writer : public teca_algorithm
{
public:
    TECA_ALGORITHM_STATIC_NEW(teca_vtk_cartesian_mesh_writer)
    ~teca_vtk_cartesian_mesh_writer();

    TECA_ALGORITHM_PROPERTY(std::string, base_file_name)

protected:
    teca_vtk_cartesian_mesh_writer();

private:

    const_p_teca_dataset execute(
        unsigned int port,
        const std::vector<const_p_teca_dataset> &input_data,
        const teca_metadata &request) override;

private:
    std::string base_file_name;
};

#endif
