/// Small client for testing. Runs either routines for the one or two camera setup with constants as configured in this file.
///
#include "OneCameraSpherical.hpp"
#include "Utils.hpp"

#include <boost/tokenizer.hpp>

#include <chrono>
#include "GenericCalibration.hpp"
#include "InputOutputHelpers.hpp"
#include "TwoCameraSpherical.hpp"

using namespace gazeestimation;

using OurCalibrationType = GenericCalibration<EyeAndCameraParameters, PupilCenterGlintInputs, DefaultGazeEstimationResult>;

void run_onecamera();
void run_twocamera();

int main()
{
	run_onecamera();
	return 0;
}

Vec3 calculate_point_of_interest(const Vec3& cornea_center, const Vec3& visual_axis_unit_vector, double z_shift)
{
	const double kg = (z_shift - cornea_center[2]) / visual_axis_unit_vector[2];
	return cornea_center + kg * visual_axis_unit_vector;
}

Vec2 estimate_screen_point(const Vec3& poi, double screen_pixel_size_x, double screen_pixel_size_y)
{
	return make_vec2(poi[0] / screen_pixel_size_x, -poi[1] / screen_pixel_size_y);
}

/// Adapter from the return value of the calibration to the format that the optimization backend dictates
/// for the variables. This enables the use of the code that applies a set of variables to the input parameters
/// to apply the final result as well.
///
/// This relies on the (de-)allocation behavior of the vector class. No memory is allocated,
/// and the returned pointers are only valid as long as no operation that changes the layout of any of the original
/// vectors is made.
const double* const* const vecvec_to_pointer_pointer(std::vector<std::vector<double>>& a) {
	std::vector<double*> tmp;
	for (unsigned int i = 0; i < a.size(); i++)
	{
		tmp.push_back(&a[i][0]);
	}
	return &tmp[0];
}

/// Calibrates against alpha, beta, R, K, camera_rotation_y, camera_rotation_z
EyeAndCameraParameters six_variable_calibration_applicator(EyeAndCameraParameters params, double const* const* variables)
{
	params.alpha = variables[0][0];
	params.beta = variables[1][0];
	params.R = variables[2][0];
	params.K = variables[3][0];
	params.cameras[0].set_camera_angle_y(variables[4][0]);
	params.cameras[0].set_camera_angle_z(variables[5][0]);
	return params;
}


Vec3 result_processor(const DefaultGazeEstimationResult& result, double z_shift, const Vec3& wcs_offset)
{
	return calculate_point_of_interest(result.center_of_cornea, result.visual_axis, z_shift) - wcs_offset;
	
}

void run_onecamera()
{
	auto calibration_data = read_input_file(L"input_calibration.txt");
	auto test_data = read_input_file(L"input_test.txt");

//	for(int i = 0; i < 10000; i++)
//	{
//		test_data.push_back(test_data[0]);
//	}

	OneCamSphericalGE gazeEstimation = OneCamSphericalGE();

	EyeAndCameraParameters parameters;
	parameters.alpha = deg_to_rad(-5);
	parameters.beta = deg_to_rad(1.5);
	parameters.R = 0.78;
	parameters.K = 0.42;
	parameters.n1 = 1.3375;
	parameters.n2 = 1;
	parameters.D = 0.53;

	// keeping in mind that wcs has its origin at the camera position for these
	const Vec3 actual_camera_position = make_vec3(24.5, -35, 10);
	const Vec3 wcs_offset = -make_vec3(24.5, -35, 10);


	PinholeCameraModel camera;
	camera.principal_point_x = 299.5;
	camera.principal_point_y = 399.5;
	camera.pixel_size_cm_x = 2.4 * 1e-6;
	camera.pixel_size_cm_y = 2.4 * 1e-6;
	camera.effective_focal_length_cm = 0.0119144;
	camera.position = actual_camera_position + wcs_offset;
	camera.set_camera_angles(deg_to_rad(8), 0, 0);
	parameters.cameras.push_back(camera);
	
	std::vector<Vec3> lights;
	lights.push_back(actual_camera_position + make_vec3(13, 0, 0) + wcs_offset);
	lights.push_back(actual_camera_position + make_vec3(-13, 0, 0) + wcs_offset);
	parameters.light_positions = lights;

	parameters.distance_to_camera_estimate = 10;

	// additional scene parameters to get poi in pixels
	const double display_surface_size_cm_x = 48.7; 
	const double display_surface_size_cm_y = 27.4;
	const double screen_resolution_x = 1680;
	const double screen_resolution_y = 1050;

	const double screen_pixel_size_x = display_surface_size_cm_x / screen_resolution_x;
	const double screen_pixel_size_y = display_surface_size_cm_y / screen_resolution_y;

	const double z_shift = -actual_camera_position[2];

	// ---------- calibrate
	OurCalibrationType calibration;
	
	/// change the true positions into world coordinate system so as to not need to carry
	/// the dependent data for that calculation into the calibration process
	std::vector<std::pair<PupilCenterGlintInputs, Vec3>> calibrate_against;
	for(auto It = calibration_data.begin(); It != calibration_data.end(); ++It)
	{
		const Vec2 true_pog = (*It).second;
		calibrate_against.push_back(std::make_pair((*It).first, 
			make_vec3(true_pog[0] * screen_pixel_size_x, -true_pog[1] * screen_pixel_size_y, 0)));
	}

	std::vector<std::vector<double>> initial_values = { 
		{parameters.alpha}, 
		{parameters.beta}, 
		{parameters.R}, 
		{parameters.K}, 
		{parameters.cameras[0].camera_angle_y()}, 
		{parameters.cameras[0].camera_angle_z()} };
	std::vector<std::vector<std::pair<double, double>>> bounds = {
		{std::make_pair(deg_to_rad(-10), deg_to_rad(10))},
		{ std::make_pair(deg_to_rad(-5), deg_to_rad(5))},
		{ std::make_pair(0.3, 2.0)},
		{ std::make_pair(0.2, 1.5)},
		{ std::make_pair(deg_to_rad(-8), deg_to_rad(8))},
		{ std::make_pair(deg_to_rad(-5), deg_to_rad(5))}
	};

	auto calibration_result = calibration.calibrate(gazeEstimation, parameters,
		six_variable_calibration_applicator,
		std::bind(result_processor, std::placeholders::_1, z_shift, wcs_offset),
		calibrate_against,
		initial_values,
		bounds);
	parameters = six_variable_calibration_applicator(parameters, vecvec_to_pointer_pointer(calibration_result));

	std::cout << "Calibration finished." << std::endl;
	std::cout << "Alpha: " << parameters.alpha << " (" << rad_to_deg(parameters.alpha) << ")" << std::endl;
	std::cout << "Beta: " << parameters.beta << " (" << rad_to_deg(parameters.beta) << ")" << std::endl;
	std::cout << "R: " << parameters.R << std::endl;
	std::cout << "K: " << parameters.K << std::endl;
	std::cout << "CamAy: " << parameters.cameras[0].camera_angle_y() << std::endl;
	std::cout << "CamAz: " << parameters.cameras[0].camera_angle_z() << std::endl;



	// ----------- estimate
	std::chrono::high_resolution_clock::time_point start_time = std::chrono::high_resolution_clock::now();

	std::vector<double> errors_pixels;
	std::vector<double> errors_cm;
	std::vector<Vec2> estimates;
	for(auto It = test_data.begin(); It != test_data.end(); ++It)
	{
		const PupilCenterGlintInputs data_in = (*It).first;
		const Vec2 true_pog = (*It).second;

		const DefaultGazeEstimationResult result = gazeEstimation.estimate(data_in, parameters);

		//std::cout << gaze_estimation_result_to_string(result);

		const Vec3 poi_gecs = calculate_point_of_interest(result.center_of_cornea, result.visual_axis, z_shift);
		const Vec3 poi_wcs = poi_gecs - wcs_offset;

		//std::cout << "POI\t\t: " << vec3toString(poi_wcs) << std::endl;

		const Vec2 pos_on_screen = estimate_screen_point(poi_wcs, screen_pixel_size_x, screen_pixel_size_y);
		estimates.push_back(pos_on_screen);

		const Vec2 delta_pixels = pos_on_screen - true_pog;
		const Vec2 delta_cm = make_vec2(true_pog[0] * screen_pixel_size_x, -true_pog[1] * screen_pixel_size_y) - make_vec2(poi_wcs[0], poi_wcs[1]);
		errors_pixels.push_back(length(delta_pixels));
		errors_cm.push_back(length(delta_cm));
	}


	std::chrono::high_resolution_clock::time_point end_time = std::chrono::high_resolution_clock::now();
	const auto time_taken_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

	std::cout << "POI's true vs actual:" << std::endl;
	for(unsigned int i = 0; i < test_data.size(); i++)
	{
	//	std::cout << vec2toString(test_data[i].second) << "\t|\t" << vec2toString(estimates[i]) << std::endl;
	}

	const double avg_error_pixels = std::accumulate(errors_pixels.begin(), errors_pixels.end(), 0.) / static_cast<double>(errors_pixels.size());
	const double avg_error_cm = std::accumulate(errors_cm.begin(), errors_cm.end(), 0.) / static_cast<double>(errors_cm.size());

	std::cout << "avg error pixels\t" << avg_error_pixels << std::endl;
	std::cout << "avg error cm\t" << avg_error_cm << std::endl;

	std::cout << "time in ms: \t" << time_taken_ms << std::endl;

	const double time_per_estimate_us = (static_cast<double>(time_taken_ms) / static_cast<double>(test_data.size())) * 1.e3;
	const double fps_upper_limit = 1 / (time_per_estimate_us/1.e6);
	std::cout << "time per estimate micro-s: \t" <<  time_per_estimate_us  << " (f: " << fps_upper_limit << ")" << std::endl;

	std::cin.get();

}




void run_twocamera()
{
	//auto calibration_data = read_input_file(L"input_calibration.txt");
	auto test_data = read_input_file_twocameras(L"E:/output_generated.csv");

	std::cout << "test data size: " << test_data.size() << std::endl;

	TwoCamSphericalGE gazeEstimation = TwoCamSphericalGE(TwoCamSphericalGE::ExplicitRefraction2);

	EyeAndCameraParameters parameters;
	parameters.alpha = deg_to_rad(-5);
	parameters.beta = deg_to_rad(1.5);
	parameters.R = 0.78;
	parameters.K = 0.42;
	parameters.n1 = 1.3375;
	parameters.n2 = 1;
	parameters.D = 0.53;

	// keeping in mind that wcs has its origin at the camera position for these
	const Vec3 wcs_offset = make_vec3(0, 0, 0);//-make_vec3(24.5, -35, 10);


	{
		PinholeCameraModel camera;
		camera.principal_point_x = 695.5;
		camera.principal_point_y = 449.5;
		camera.pixel_size_cm_x = 4.65 * 1e-6;
		camera.pixel_size_cm_y = 4.65 * 1e-6;
		camera.effective_focal_length_cm = 0.0350170102672;
		const Vec3 actual_camera_position = make_vec3(-10,-21,2);
		camera.position = actual_camera_position + wcs_offset;
		camera.set_camera_angles(deg_to_rad(-27.70716514), deg_to_rad(9.01932243), 0);
		std::cout << camera.rotation_matrix() << std::endl;
		parameters.cameras.push_back(camera);
	}
	{
		PinholeCameraModel camera;
		camera.principal_point_x = 695.5;
		camera.principal_point_y = 449.5;
		camera.pixel_size_cm_x = 4.65 * 1e-6;
		camera.pixel_size_cm_y = 4.65 * 1e-6;
		camera.effective_focal_length_cm = 0.0350170102672;
		const Vec3 actual_camera_position = make_vec3(10,-21,2);
		camera.position = actual_camera_position + wcs_offset;
		camera.set_camera_angles(deg_to_rad(-27.70716514), deg_to_rad(-9.01932243), 0);
		parameters.cameras.push_back(camera);
	}

	parameters.distance_to_camera_estimate = 100;

	std::vector<Vec3> lights;
	lights.push_back(make_vec3(-25, 10, 0) + wcs_offset);
	lights.push_back(make_vec3(25, 10, 0) + wcs_offset);
	parameters.light_positions = lights;

	const double z_shift = 0;

	// ----------- estimate
	std::chrono::high_resolution_clock::time_point start_time = std::chrono::high_resolution_clock::now();

	std::vector<double> errors_cm;
	std::vector<Vec2> estimates;
	for (auto It = test_data.begin(); It != test_data.end(); ++It)
	{
		const PupilCenterGlintInputs data_in = (*It).first;
		const Vec2 true_pog = (*It).second;

		const DefaultGazeEstimationResult result = gazeEstimation.estimate(data_in, parameters);

		const Vec3 poi_gecs = calculate_point_of_interest(result.center_of_cornea, result.visual_axis, z_shift);
		const Vec3 poi_wcs = poi_gecs - wcs_offset;

		//std::cout << "POI\t\t: " << vec3toString(poi_wcs) << " vs " << true_pog[0] << ", " << true_pog[1] << std::endl;
		
		Vec2 delta_cm = make_vec2(poi_wcs[0], poi_wcs[1]) - true_pog;
		errors_cm.push_back(length(delta_cm));

		//std::cout << length(delta_cm) << std::endl;
	}
	
	std::chrono::high_resolution_clock::time_point end_time = std::chrono::high_resolution_clock::now();
	const auto time_taken_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

	std::cout << "POI's true vs actual:" << std::endl;
	for (unsigned int i = 0; i < test_data.size(); i++)
	{
		//std::cout << vec3toString(test_data[i].second) << "\t|\t" << vec3toString(estimates[i]) << std::endl;
	}

	const double avg_error_cm = std::accumulate(errors_cm.begin(), errors_cm.end(), 0.) / static_cast<double>(errors_cm.size());

	std::cout << "avg error cm\t" << avg_error_cm << std::endl;

	std::cout << "time in ms: \t" << time_taken_ms << std::endl;

	const double time_per_estimate_us = (static_cast<double>(time_taken_ms) / static_cast<double>(test_data.size())) * 1.e3;
	const double fps_upper_limit = 1 / (time_per_estimate_us / 1.e6);
	std::cout << "time per estimate micro-s: \t" << time_per_estimate_us << " (f: " << fps_upper_limit << ")" << std::endl;
	
	std::cin.get();

}


