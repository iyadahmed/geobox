#include <algorithm> // for std::erase
#include <cmath>
#include <cstdint>
#include <cstdlib> // for std::exit
#include <format>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <random> // for std::discrete_distribution
#include <string>
#include <utility>
#include <vector>

#define GLFW_INCLUDE_NONE
#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <ImGuiFileDialog.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/io.hpp>

#include "bvh.hpp"
#include "geobox_app.hpp"
#include "geobox_exceptions.hpp"
#include "point_cloud_object.hpp"
#include "read_stl.hpp"
#include "shader.hpp"
#include "triangle.hpp"

#ifdef ENABLE_SUPERLUMINAL_PERF_API
#include <Superluminal/PerformanceAPI.h>
#endif

constexpr int INIT_WINDOW_WIDTH = 800;
constexpr int INIT_WINDOW_HEIGHT = 600;
constexpr const char *WINDOW_TITLE = "GeoBox";

constexpr const char *LOAD_STL_DIALOG_KEY = "Load_STL_Dialog_Key";
constexpr const char *LOAD_STL_BUTTON_AND_DIALOG_TITLE = "Load .stl";

constexpr ImVec2 INITIAL_IMGUI_FILE_DIALOG_WINDOW_OFFSET(100, 100);
constexpr ImVec2 INITIAL_IMGUI_FILE_DIALOG_WINDOW_SIZE(600, 500);

constexpr float CAMERA_ORBIT_ZOOM_SPEED_MULTIPLIER = 2.5f;
constexpr float CAMERA_PAN_SPEED_MULTIPLIER = 0.05f;
constexpr float CAMERA_ORBIT_ROTATE_SPEED_RADIANS = glm::radians(20.0f);
// Minimum value for orbit radius when calculating various camera movement speeds based on orbit radius
constexpr float MIN_ORBIT_RADIUS_AS_SPEED_MULTIPLIER = 0.1f;

constexpr int NUM_ANTIALIASING_SAMPLES = 8;

constexpr float DEFAULT_POINT_SIZE = 6.0f;

GeoBox_App::GeoBox_App() {
  init_glfw();
  init_glad();
  init_glfw_callbacks();
  // NOTE: it is important to initialize ImGUI after initializing GLFW callbacks, otherwise setting GLFW callbacks will
  // override ImGUI callbacks, ImGUI smartly chains its callbacks to any existing callbacks, so we initialize our
  // callbacks first, and let ImGUI chain its callbacks
  init_imgui();
  if (!init_shaders()) {
    std::cerr << "Failed to initialize shaders" << std::endl;
    shutdown();
    std::exit(-1);
  }

  // Enable depth testing
  glEnable(GL_DEPTH_TEST);

  // Set OpenGL viewport
  int width;
  int height;
  glfwGetFramebufferSize(m_window, &width, &height);
  glViewport(0, 0, width, height);

  // Enable anti-aliasing
  glEnable(GL_MULTISAMPLE);

  // Set point size
  glPointSize(DEFAULT_POINT_SIZE);

  // Enable polygon depth offset, useful for avoiding z-fighting in specific scenarios (e.g. rendering point cloud and
  // wireframe overlays)
  glEnable(GL_POLYGON_OFFSET_FILL);
}

void GeoBox_App::main_loop() {
  while (!glfwWindowShouldClose(m_window)) {
    // Update delta time
    auto current_frame_time = static_cast<float>(glfwGetTime());
    m_delta_time = current_frame_time - m_last_frame_time;
    m_last_frame_time = current_frame_time;

    // Poll events
    glfwPollEvents();

    // Process Input
    process_input();

    // Update state

    // Render to framebuffer
    render();

    // Swap/Present framebuffer to screen
    glfwSwapBuffers(m_window);

    // Delay to control FPS if needed
  }
  shutdown();
}

void GeoBox_App::init_glfw() {
  glfwInit();
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
  glfwWindowHint(GLFW_SAMPLES, NUM_ANTIALIASING_SAMPLES);

#ifdef __APPLE__
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

  m_window = glfwCreateWindow(INIT_WINDOW_WIDTH, INIT_WINDOW_HEIGHT, WINDOW_TITLE, nullptr, nullptr);
  if (m_window == nullptr) {
    std::cerr << "Failed to create GLFW window" << std::endl;
    glfwTerminate();
    std::exit(-1);
  }
  glfwMakeContextCurrent(m_window);

  // Store a pointer to the object in the GLFW "user" pointer,
  // so we can access methods in GLFW callbacks, see
  // https://stackoverflow.com/a/28660673/8094047
  glfwSetWindowUserPointer(m_window, this);

  // Raw mouse motion, from GLFW documentation:
  // "Raw mouse motion is closer to the actual motion of the mouse across a surface. It is not affected by the scaling
  // and acceleration applied to the motion of the desktop cursor. That processing is suitable for a cursor while raw
  // motion is better for controlling for example a 3D camera. Because of this, raw mouse motion is only provided when
  // the cursor is disabled."
  if (glfwRawMouseMotionSupported()) {
    glfwSetInputMode(m_window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
  }
}

void GeoBox_App::init_glad() {
  if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
    std::cerr << "Failed to initialize GLAD" << std::endl;
    glfwTerminate();
    std::exit(-1);
  }
}

void GeoBox_App::init_imgui() {
  // Setup Dear ImGui context
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  // Disabling ini file: https://github.com/ocornut/imgui/issues/5169
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;  // Enable Gamepad Controls
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;     // Enable docking (only supported in ImGUI docking branch)

  // Setup Platform/Renderer backends
  ImGui_ImplGlfw_InitForOpenGL(m_window, true); // Second param install_callback=true will install GLFW
                                                // callbacks and chain to existing ones.
  ImGui_ImplOpenGL3_Init();
}

bool GeoBox_App::init_shaders() {
  try {
    m_phong_shader = std::make_shared<Shader>("resources/shaders/phong.vert", "resources/shaders/phong.frag");
    m_point_cloud_shader =
        std::make_shared<Shader>("resources/shaders/point_cloud.vert", "resources/shaders/point_cloud.frag");
  } catch (const GeoBox_Error &) {
    return false;
  }
  return true;
}

void framebuffer_size_callback(const GLFWwindow * /*window*/, int width, int height) {
  glViewport(0, 0, width, height);
}

void key_callback(GLFWwindow *window, int key, int /*scancode*/, int action, int mods) {
  if (const ImGuiIO &imgui_io = ImGui::GetIO(); imgui_io.WantCaptureKeyboard)
    return;
  auto app = static_cast<GeoBox_App *>(glfwGetWindowUserPointer(window));
  if (key == GLFW_KEY_Z && action == GLFW_PRESS) {
    if (mods & GLFW_MOD_SHIFT) {
      app->redo();
    } else {
      app->undo();
    }
  }
}

void GeoBox_App::init_glfw_callbacks() {
  glfwSetFramebufferSizeCallback(m_window, (GLFWframebuffersizefun)framebuffer_size_callback);
  glfwSetKeyCallback(m_window, (GLFWkeyfun)key_callback);
}

void GeoBox_App::process_input() {
  if (const ImGuiIO &imgui_io = ImGui::GetIO(); imgui_io.WantCaptureMouse || imgui_io.WantCaptureKeyboard)
    return;
  float orbit_radius_as_speed_multiplier = glm::max(m_camera.get_orbit_radius(), MIN_ORBIT_RADIUS_AS_SPEED_MULTIPLIER);
  float camera_orbit_zoom_speed = CAMERA_ORBIT_ZOOM_SPEED_MULTIPLIER * orbit_radius_as_speed_multiplier * m_delta_time;
  float camera_orbit_zoom_offset = 0.0f;
  glm::vec3 camera_orbit_origin_offset(0.0f);
  float camera_inclination_offset = 0.0f;
  float camera_azimuth_offset = 0.0f;

  if (glfwGetKey(m_window, GLFW_KEY_W) == GLFW_PRESS) {
    camera_orbit_zoom_offset = -1.0f * camera_orbit_zoom_speed;
  }
  if (glfwGetKey(m_window, GLFW_KEY_S) == GLFW_PRESS) {
    camera_orbit_zoom_offset = camera_orbit_zoom_speed;
  }
  if (glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
    glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    double x_pos;
    double y_pos;
    glfwGetCursorPos(m_window, &x_pos, &y_pos);
    if (m_last_mouse_pos.has_value()) {
      float x_offset = static_cast<float>(x_pos) - m_last_mouse_pos->x;
      // reversed since y-coordinates range from bottom to top
      float y_offset = m_last_mouse_pos->y - static_cast<float>(y_pos);

      if (glfwGetKey(m_window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) {
        // We negate the expression because we are moving the camera, and we want the scene to follow the mouse
        // movement, so we negate the camera movement to achieve that
        camera_orbit_origin_offset = -1.0f * (m_camera.get_right() * x_offset + m_camera.get_up() * y_offset) *
                                     CAMERA_PAN_SPEED_MULTIPLIER * m_delta_time * orbit_radius_as_speed_multiplier;
      } else {
        float orbit_speed = CAMERA_ORBIT_ROTATE_SPEED_RADIANS * m_delta_time;
        // if y_offset increases from bottom to top we increase inclination in the same direction,
        // increasing inclination angle lowers camera position, which is what we want,
        // we want the scene to orbit up when mouse is dragged up,
        // so we orbit the camera down to achieve the effect of orbiting the scene up
        camera_inclination_offset = y_offset * orbit_speed;
        // if x_offset increases from left to right, we want to decrease the camera's azimuth (also called polar angle),
        // this will cause the camera to rotate left around the orbit origin,
        // so that when the mouse moves right, the scene also appears rotating right,
        // which is what we want
        camera_azimuth_offset = -1.0f * x_offset * orbit_speed;
      }
    }
    m_last_mouse_pos = glm::vec2(x_pos, y_pos);
  } else {
    m_last_mouse_pos.reset();
    glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
  }
  m_camera.update(camera_inclination_offset, camera_azimuth_offset, camera_orbit_zoom_offset,
                  camera_orbit_origin_offset);
}

std::vector<glm::vec3> GeoBox_App::generate_points_on_surface() {
  std::vector<glm::vec3> points;
  for (const std::shared_ptr<Indexed_Triangle_Mesh_Object> &object : m_objects) {
    const std::vector<glm::vec3> &vertices = object->get_vertices();
    const std::vector<unsigned int> &indices = object->get_indices();
    const std::vector<float> &triangle_areas = object->get_triangle_areas();
    // Pick a triangle randomly
    assert(indices.size() > 0);
    assert(indices.size() % 3 == 0);
    std::discrete_distribution<size_t> random_index_distribution(triangle_areas.begin(), triangle_areas.end());
    // Sample points
    for (uint32_t i = 0; i < m_points_on_surface_count; i++) {
      size_t triangle_index = random_index_distribution(m_random_engine);
      const glm::vec3 &a = vertices[indices[triangle_index * 3 + 0]];
      const glm::vec3 &b = vertices[indices[triangle_index * 3 + 1]];
      const glm::vec3 &c = vertices[indices[triangle_index * 3 + 2]];
      glm::vec3 ab = b - a;
      glm::vec3 ac = c - a;
      glm::vec2 uv = random_triangle_barycentric_coords();
      glm::vec3 p = ab * uv.x + ac * uv.y + a;
      points.push_back(p);
    }
  }
  return points;
}

void GeoBox_App::on_generate_points_on_surface_button_click() {
  std::vector<glm::vec3> points = generate_points_on_surface();
  try {
    auto point_cloud_object = std::make_shared<Point_Cloud_Object>(points, glm::mat4(1.0f));
    m_point_cloud_objects.push_back(point_cloud_object);
    m_undo_stack.emplace(
        [point_cloud_object, this]() { std::erase(m_point_cloud_objects, point_cloud_object); }, // Undo
        [point_cloud_object, this]() { m_point_cloud_objects.push_back(point_cloud_object); }    // Redo
    );
  } catch (const GeoBox_Error &error) {
    std::cerr << error.what() << std::endl;
  }
}

void GeoBox_App::render() {
  int width;
  int height;
  glfwGetFramebufferSize(m_window, &width, &height);
  if (width == 0 || height == 0) {
    return;
  }

  glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  m_phong_shader->use();
  m_phong_shader->get_uniform_setter<glm::vec3>("object_color")({1.0f, 1.0f, 1.0f});
  m_phong_shader->get_uniform_setter<glm::vec3>("light_color")({1.0f, 1.0f, 1.0f});
  glm::mat4 view = m_camera.get_view_matrix();
  glm::mat4 projection =
      glm::perspective(glm::radians(m_perspective_fov_degrees), (float)width / (float)height, 0.01f, 1000.0f);
  m_phong_shader->get_uniform_setter<glm::mat4>("view_matrix")(view);
  m_phong_shader->get_uniform_setter<glm::mat4>("projection_matrix")(projection);
  m_phong_shader->get_uniform_setter<glm::vec3>("camera_position")(m_camera.get_camera_pos());

  // Avoid z-fighting with point clouds by pushing polygon depth away a bit
  float original_polygon_offset_factor;
  float original_polygon_offset_units;
  glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &original_polygon_offset_factor);
  glGetFloatv(GL_POLYGON_OFFSET_UNITS, &original_polygon_offset_units);
  glPolygonOffset(0.0f, 1.0f);
  {
    auto model_matrix_uniform_setter = m_phong_shader->get_uniform_setter<glm::mat4>("model_matrix");
    auto normal_matrix_uniform_setter = m_phong_shader->get_uniform_setter<glm::mat3>("normal_matrix");
    for (const std::shared_ptr<Indexed_Triangle_Mesh_Object> &object : m_objects) {
      model_matrix_uniform_setter(object->get_model_matrix());
      normal_matrix_uniform_setter(object->get_normal_matrix());
      object->draw();
    }
  }
  // Restore original polygon depth offset
  glPolygonOffset(original_polygon_offset_factor, original_polygon_offset_units);

  m_point_cloud_shader->use();
  m_point_cloud_shader->get_uniform_setter<glm::mat4>("view_matrix")(view);
  m_point_cloud_shader->get_uniform_setter<glm::mat4>("projection_matrix")(projection);
  {
    auto model_matrix_uniform_setter = m_point_cloud_shader->get_uniform_setter<glm::mat4>("model_matrix");
    for (const std::shared_ptr<Point_Cloud_Object> &point_cloud_object : m_point_cloud_objects) {
      model_matrix_uniform_setter(point_cloud_object->get_model_matrix());
      point_cloud_object->draw();
    }
  }

  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();

  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  // Thanks!: https://github.com/ocornut/imgui/issues/6307
  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("File")) {
      if (ImGui::MenuItem(LOAD_STL_BUTTON_AND_DIALOG_TITLE)) {
        IGFD::FileDialogConfig config;
        config.path = ".";
        ImGuiFileDialog::Instance()->OpenDialog(LOAD_STL_DIALOG_KEY, LOAD_STL_BUTTON_AND_DIALOG_TITLE, ".stl", config);
      }
      ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
  }
  ImGui::PopStyleVar();

  const ImGuiViewport *main_viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(main_viewport->WorkPos.x + INITIAL_IMGUI_FILE_DIALOG_WINDOW_OFFSET.x,
                                 main_viewport->WorkPos.y + INITIAL_IMGUI_FILE_DIALOG_WINDOW_OFFSET.y),
                          ImGuiCond_Once);
  ImGui::SetNextWindowSize(INITIAL_IMGUI_FILE_DIALOG_WINDOW_SIZE, ImGuiCond_Once);
  if (ImGuiFileDialog::Instance()->Display(LOAD_STL_DIALOG_KEY)) {
    if (ImGuiFileDialog::Instance()->IsOk()) {
      std::string file_path = ImGuiFileDialog::Instance()->GetFilePathName();
      on_load_stl_dialog_ok(file_path);
    }
    ImGuiFileDialog::Instance()->Close();
  }

  ImGui::SetNextWindowPos(ImVec2(main_viewport->WorkPos.x, main_viewport->WorkPos.y), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(main_viewport->WorkSize.x / 5, main_viewport->WorkSize.y), ImGuiCond_Always);
  ImGui::Begin("Operations", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
  if (ImGui::CollapsingHeader("Points In Volume", ImGuiTreeNodeFlags_DefaultOpen)) {
    // TODO
  }
  if (ImGui::CollapsingHeader("Mesh Offset", ImGuiTreeNodeFlags_DefaultOpen)) {
    // TODO
  }
  if (ImGui::CollapsingHeader("Points On Surface", ImGuiTreeNodeFlags_DefaultOpen)) {
    uint32_t step = 1;
    uint32_t step_fast = 10;
    ImGui::InputScalar("Count", ImGuiDataType_U32, &m_points_on_surface_count, &step, &step_fast);
    if (ImGui::Button("Generate")) {
      on_generate_points_on_surface_button_click();
    }
  }
  ImGui::End();

  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void GeoBox_App::shutdown() {
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwTerminate();
}

void GeoBox_App::on_load_stl_dialog_ok(const std::string &file_path) {
#ifdef ENABLE_SUPERLUMINAL_PERF_API
  PerformanceAPI_BeginEvent(__FUNCTION__, nullptr, PERFORMANCEAPI_DEFAULT_COLOR);
  PERFORMANCEAPI_INSTRUMENT_FUNCTION();
#endif
  std::optional<std::vector<Triangle>> triangles = read_stl_mesh_file(file_path);
  if (!triangles.has_value()) {
    std::cerr << "Failed to import .stl mesh file: " << file_path << std::endl;
    return;
  }
  if (triangles->empty()) {
    std::cerr << "Empty mesh: " << file_path << std::endl;
    return;
  }

  try {
    auto object = std::make_shared<Indexed_Triangle_Mesh_Object>(triangles.value(), glm::mat4(1.0f));
    m_objects.push_back(object);
    m_undo_stack.emplace([object, this]() { std::erase(m_objects, object); }, // Undo
                         [object, this]() { m_objects.push_back(object); }    // Redo
    );
  } catch (const GeoBox_Error &error) {
    std::cerr << error.what() << std::endl;
    std::cerr << "Failed to create object" << std::endl;
  }

#ifdef ENABLE_SUPERLUMINAL_PERF_API
  PerformanceAPI_EndEvent();
#endif
}
