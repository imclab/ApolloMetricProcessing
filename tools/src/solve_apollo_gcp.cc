#include <vw/Camera/CameraGeometry.h>
#include <vw/BundleAdjustment/ControlNetwork.h>
#include <vw/Cartography/SimplePointImageManipulation.h>
#include <asp/IsisIO/IsisAdjustCameraModel.h>
#include <boost/filesystem/path.hpp>
#include <boost/program_options.hpp>

namespace po = boost::program_options;
namespace fs = boost::filesystem;

using namespace vw;
using namespace vw::camera;

#include "camera_solve.h"

int main( int argc, char* argv[] ) {

  std::string cube_file, cnet_file;
  po::options_description general_options("Options");
  general_options.add_options()
    ("cube-file", po::value(&cube_file), "Input cube file.")
    ("cnet-file", po::value(&cnet_file), "Input cnet file.")
    ("verbose,v", "Display more debug information.")
    ("error-only","Display only the cnet and the projective error.")
    ("help,h", "Display this help message");

  po::positional_options_description p;
  p.add("cube-file", 1);
  p.add("cnet-file", 1);

  po::variables_map vm;
  po::store( po::command_line_parser( argc, argv ).options(general_options).positional(p).run(), vm );
  po::notify( vm );

  std::ostringstream usage;
  usage << "Usage: " << argv[0] << " [options] <cube file> <cnet file>\n\n";
  usage << general_options << std::endl;

  if ( vm.count("help") || cnet_file.empty() || cube_file.empty() ) {
    vw_out() << usage.str() << std::endl;
    return 1;
  }

  // Get Control Network
  ba::ControlNetwork cnet( cnet_file, ba::FmtBinary );

  // Get Camera Model
  std::string adjust_file =
    fs::path( cube_file ).replace_extension("isis_adjust").string();
  vw_out() << "Loading \"" << adjust_file << "\"\n";
  boost::shared_ptr<IsisAdjustCameraModel> camera;
  boost::shared_ptr<asp::BaseEquation> posF, poseF;
  {
    std::ifstream input( adjust_file.c_str() );
    posF = asp::read_equation(input);
    poseF = asp::read_equation(input);
    camera.reset( new IsisAdjustCameraModel( cube_file, posF, poseF ) );
  }
  std::string serial = camera->serial_number();

  // double check that cnet belongs to serial?
  std::vector<const ba::ControlPoint*> matching_cp;
  BOOST_FOREACH( ba::ControlPoint const& cp, cnet ) {
    if ( cp.type() != ba::ControlPoint::GroundControlPoint )
      continue;
    bool found = false;
    BOOST_FOREACH( ba::ControlMeasure const& cm, cp ) {
      if ( cm.serial() == serial || cm.serial() == cube_file ||
           cm.description() == serial || cm.description() == cube_file ) {
        found = true;
        break;
      }
    }
    if ( found )
      matching_cp.push_back( &cp );
  }
  if ( !matching_cp.size() )
    vw_throw( IOErr() << "Unable to find matching control points!" );

  // Create a control network that has the subset of measurements that
  // apply to this camera.
  ba::ControlNetwork cnet_gcp("Subset");
  BOOST_FOREACH( ba::ControlPoint const* cp, matching_cp ) {
    ba::ControlPoint new_cp(ba::ControlPoint::GroundControlPoint);
    new_cp.set_position( cp->position() );
    BOOST_FOREACH( ba::ControlMeasure const& cm, *cp ) {
      if ( cm.serial() == serial || cm.serial() == cube_file ||
           cm.description() == serial || cm.description() == cube_file ) {
        new_cp.add_measure( cm );
        break;
      }
    }
    cnet_gcp.add_control_point( new_cp );
  }

  // Verbose an debugging to see if the LOLA lookup is actually working.
  if ( vm.count("verbose") ) {
    BOOST_FOREACH( ba::ControlPoint& cp, cnet_gcp ) {
      Vector3 lla = cartography::xyz_to_lon_lat_radius(cp.position()) - Vector3(0,0,1737400);
      vw_out() << "\t" << lla << " -> " << cp[0].position() << "\n";
    }
  }

  // Let's attempt to solve for the camera from GCP
  CameraGCPLMA lma_model( cnet_gcp, camera );
  Vector<double> seed = lma_model.extract( camera );
  double starting_error = norm_2(lma_model(seed)) /cnet_gcp.size();
  if ( vm.count("error-only") ) {
    vw_out() << cnet_file << " -> " << starting_error << " px\n";
    return 0;
  }
  std::cout << "-> Attempt with seed: " << seed << "\n";
  std::cout << "-> Starting error: " << starting_error << " px\n";
  int status = 0;
  Vector<double> objective;
  objective.set_size( cnet_gcp.size() * 2 );
  Vector<double> result = levenberg_marquardt( lma_model, seed,
                                               objective, status );
  double error = norm_2(lma_model(result)) / cnet_gcp.size();
  std::cout << "-> lma ending error: " << error << "\n";
  double dlt_error = error;

  if ( status < 1 || error > 2 ) {
    std::cout << "Zero state restart:\n";
    // Attempt with the original Apollo measurements

    seed = Vector<double>( seed.size() ); // zero out
    std::cout << "-> Attempt with seed: " << seed << "\n";
    std::cout << "-> Starting error: " << sum(abs(lma_model(seed))) << " px\n";
    result = levenberg_marquardt( lma_model, seed,
                                  objective, status );
    error = norm_2(lma_model(result)) / cnet_gcp.size();
    std::cout << "-> lma ending error: " << error << "\n";
  }

  if ( (status < 1 || error > 2) && fabs(error - dlt_error) > 1e-2 ) {
    std::cout << "DLT:\n";
    std::vector<Vector<double> > point_meas, image_meas;
    point_meas.reserve( cnet_gcp.size() );
    image_meas.reserve( cnet_gcp.size() );
    BOOST_FOREACH( ba::ControlPoint const& cp, cnet_gcp ) {
      point_meas.push_back( Vector<double,4>() );
      subvector(point_meas.back(),0,3) = cp.position();
      point_meas.back()[3] = 1;
      image_meas.push_back( Vector<double,3>() );
      subvector(image_meas.back(),0,2) = cp[0].position();
      image_meas.back()[2] = 1;
    }

    // Attempt a DLT method with some LMA refinement
    camera::CameraMatrixFittingFunctor fit_func;
    Matrix<double> projective_dlt =
      fit_func(point_meas,image_meas);
    Vector<double> camera_center = select_col(nullspace(projective_dlt),0);
    camera_center = subvector(camera_center,0,3)/camera_center[3];
    Matrix<double> K,rotation;
    rqd(submatrix(projective_dlt,0,0,3,3),K,rotation);

    // Find delta equations
    (*posF)[0] = (*posF)[1] = (*posF)[2] = 0;
    (*poseF)[0] = (*poseF)[1] = (*poseF)[2] = 0;
    Vector<double> position_delta = camera_center - camera->camera_center(Vector2());
    Vector<double> pose_delta = (-Quat((camera->camera_pose(Vector2()).rotation_matrix())*inverse(rotation))).axis_angle();
    (*posF)[0] = position_delta[0];
    (*posF)[1] = position_delta[1];
    (*posF)[2] = position_delta[2];
    (*poseF)[0] = pose_delta[0];
    (*poseF)[1] = pose_delta[1];
    (*poseF)[2] = pose_delta[2];

    // Seed DLT to camera solve
    seed = lma_model.extract( camera );
    std::cout << "-> Attempt with seed: " << seed << "\n";
    std::cout << "-> Starting error: " << sum(abs(lma_model(seed))) << " px\n";
    result = levenberg_marquardt( lma_model, seed,
                                  objective, status );
    error = norm_2(lma_model(result)) / cnet_gcp.size();
    std::cout << "-> lma ending error: " << error << "\n";
  }

  if ( status > 0 )
    std::cout << "Converged! Status: " << status << "\n";
  else
    std::cout << "Failed to convege! Status: " << status << "\n";
  std::cout << "Final error : " << error << " px\n"; 

  std::cout << "Delta: " << result - seed << "\n";

  // Apply new solution to the equations and then write them back out
  (*posF)[0] = result[0];
  (*posF)[1] = result[1];
  (*posF)[2] = result[2];
  (*poseF)[0] = result[3];
  (*poseF)[1] = result[4];
  (*poseF)[2] = result[5];

  if ( status > 0 && (error < 2 || starting_error / error > 50 || fabs(dlt_error - error) < 1e-2 ) ) {
    std::cout << "Writing isis_adjust.\n";
    std::ofstream output( adjust_file.c_str() );
    asp::write_equation(output, posF);
    asp::write_equation(output, poseF);
    output.close();
  } else {
    std::cout << "Failed to produce solution worth writing.\n";
  }

  return 0;
}
