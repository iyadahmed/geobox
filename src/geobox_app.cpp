#include <iostream>
#include <string>
#include <fstream>
#include <array>
#include <cstdlib> // for std::exit
#include <vector>
#include <optional>
#include <iterator> // for std::istreambuf_iterator
#include <limits>
#include <cmath>
#include <numbers> // for std::numbers::pi

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "ImGuiFileDialog.h"

#include "mat4x4f.hpp"
#include "vec3f.hpp"
#include "geobox_app.hpp"

constexpr int INIT_WINDOW_WIDTH = 800;
constexpr int INIT_WINDOW_HEIGHT = 600;
constexpr const char *WINDOW_TITLE = "GeoBox";

constexpr const char *IMGUI_MAIN_WINDOW_TITLE = WINDOW_TITLE;
constexpr ImVec2 INITIAL_IMGUI_MAIN_WINDOW_SIZE(550, 680);

constexpr const char *LOAD_STL_DIALOG_KEY = "Load_STL_Dialog_Key";
constexpr const char *LOAD_STL_BUTTON_DIALOG_TITLE = "Load .stl";

constexpr size_t BINARY_STL_HEADER_SIZE = 80;

struct Triangle
{
    Vec3f normal;
    std::array<Vec3f, 3> vertices;
};

struct Indexed_Mesh
{
    struct Triangle
    {
        uint32_t vertices[3];
    };
    std::vector<Vec3f> vertices;
    std::vector<Triangle> triangles;

    GPU_Mesh to_gpu()
    {
        unsigned int VAO;
        glGenVertexArrays(1, &VAO);
        glBindVertexArray(VAO);

        unsigned int VBO;
        glGenBuffers(1, &VBO);
        glBindBuffer(GL_ARRAY_BUFFER, VBO);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vec3f), (float *)vertices.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float[3]), (void *)0);
        glEnableVertexAttribArray(0);

        unsigned int EBO;
        glGenBuffers(1, &EBO);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);

        glBufferData(GL_ELEMENT_ARRAY_BUFFER, triangles.size() * sizeof(Triangle), triangles.data(), GL_STATIC_DRAW);

        return GPU_Mesh(VAO, triangles.size() * 3, true);
    }
};

static Indexed_Mesh generate_box(const Vec3f &min, const Vec3f &max)
{
    Indexed_Mesh mesh;
    mesh.vertices = {min,
                     {max.x, min.y, min.z},
                     {max.x, max.y, min.z},
                     {min.x, max.y, min.z},
                     {min.x, min.y, max.z},
                     {max.x, min.y, max.z},
                     max,
                     {min.x, max.y, max.z}};
    mesh.triangles = {{0, 1, 2},
                      {0, 2, 3},
                      {1, 5, 6},
                      {1, 6, 2},
                      {5, 4, 7},
                      {5, 7, 6},
                      {4, 0, 3},
                      {4, 3, 7},
                      {3, 2, 6},
                      {3, 6, 7},
                      {4, 5, 1},
                      {4, 1, 0}};
    return mesh;
}

static float degrees_to_radians(float degrees)
{
    return degrees * (std::numbers::pi / 180.0f);
}

[[maybe_unused]] static size_t calc_file_size(std::ifstream &ifs)
{
    auto original_pos = ifs.tellg();
    ifs.seekg(0, std::ifstream::end);
    auto end = ifs.tellg();
    ifs.seekg(0, std::ifstream::beg);
    size_t file_size = end - ifs.tellg();
    ifs.seekg(original_pos, std::ifstream::beg);
    return file_size;
}

static size_t calc_expected_binary_stl_mesh_file_size(uint32_t num_triangles)
{
    static_assert(sizeof(Triangle) == sizeof(float[4][3]), "Incorrect size for Triangle struct"); // Normal + 3 vertices = 4 * 3 floats
    return BINARY_STL_HEADER_SIZE + sizeof(uint32_t) + num_triangles * (sizeof(Triangle) + sizeof(uint16_t));
}

static std::vector<Vec3f> read_stl_mesh_file_binary(std::ifstream &ifs, uint32_t num_triangles)
{
    Triangle t;
    std::vector<Vec3f> vertices;
    vertices.reserve(num_triangles * 3);
    for (uint32_t i = 0; i < num_triangles; i++)
    {
        ifs.read((char *)&t, sizeof(Triangle));
        // Skip "attribute byte count"
        ifs.seekg(sizeof(uint16_t), std::ifstream::cur);

        vertices.insert(vertices.end(), t.vertices.begin(), t.vertices.end());
    }
    return vertices;
}

static std::vector<Vec3f> read_stl_mesh_file_ascii(std::ifstream &ifs)
{
    Triangle t;
    std::vector<Vec3f> vertices;
    while (ifs.good())
    {
        std::string token;
        ifs >> token;
        if (token == "facet")
        {
            ifs >> token; // expecting "normal"
            ifs >> t.normal.x >> t.normal.y >> t.normal.z;
            ifs >> token; // expecting "outer"
            ifs >> token; // expecting "loop"
            for (int i = 0; i < 3; i++)
            {
                ifs >> token; // expecting "vertex"
                ifs >> t.vertices[i].x >> t.vertices[i].y >> t.vertices[i].z;
            }
            ifs >> token; // expecting "endloop"
            ifs >> token; // expecting "endfacet"
        }
        vertices.insert(vertices.end(), t.vertices.begin(), t.vertices.end());
    }
    return vertices;
}

static std::optional<std::vector<Vec3f>> read_stl_mesh_file(const std::string &file_path)
{
    std::ifstream ifs(file_path, std::ifstream::ate | std::ifstream::binary);
    if (!ifs.is_open())
    {
        std::cerr << "Failed to open file: " << file_path << std::endl;
        return {};
    }

    auto file_end = ifs.tellg(); // file is already at end when it is open in "ate" mode
    ifs.seekg(0, std::ifstream::beg);
    auto file_beg = ifs.tellg();
    auto file_size = file_end - file_beg;

    if (file_size == 0)
    {
        std::cerr << "Empty file: " << file_path << std::endl;
        return {};
    }
    // Assume file is binary at first and skip binary header
    ifs.seekg(BINARY_STL_HEADER_SIZE, std::ifstream::beg);

    uint32_t num_triangles = 0;
    ifs.read((char *)&num_triangles, sizeof(uint32_t));

    if (file_size == calc_expected_binary_stl_mesh_file_size(num_triangles))
    {
        return read_stl_mesh_file_binary(ifs, num_triangles);
    }
    else
    {
        ifs.seekg(0, std::ifstream::beg);
        return read_stl_mesh_file_ascii(ifs);
    }
}

static std::optional<std::string> read_file(const std::string &file_path)
{
    std::ifstream ifs(file_path);
    if (!ifs.is_open())
    {
        std::cerr << "Failed to open file: " << file_path << std::endl;
        return {};
    }
    return std::string(std::istreambuf_iterator(ifs), {});
}

GeoBox_App::GeoBox_App()
{
    init_glfw();
    init_glad();
    init_imgui();
    init_gl_viewport();
    init_glfw_callbacks();
    if (!init_shaders())
    {
        std::cerr << "Failed to initialize shaders" << std::endl;
        shutdown();
        std::exit(-1);
    }

    Indexed_Mesh box = generate_box({-1, -1, -1}, {1, 1, 1});
    m_gpu_meshes.push_back(box.to_gpu());
}

void GeoBox_App::main_loop()
{
    while (!glfwWindowShouldClose(m_window))
    {
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

void GeoBox_App::init_glfw()
{
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);

#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    m_window = glfwCreateWindow(INIT_WINDOW_WIDTH, INIT_WINDOW_HEIGHT, WINDOW_TITLE, nullptr, nullptr);
    if (m_window == nullptr)
    {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        std::exit(-1);
    }
    glfwMakeContextCurrent(m_window);

    // Store a pointer to the object in the GLFW "user" pointer,
    // so we can access methods in GLFW callbacks, see https://stackoverflow.com/a/28660673/8094047
    glfwSetWindowUserPointer(m_window, this);
}

void GeoBox_App::init_glad() const
{
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
    {
        std::cerr << "Failed to initialize GLAD" << std::endl;
        glfwTerminate();
        std::exit(-1);
    }
}

void GeoBox_App::init_imgui()
{
    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    // Disabling ini file: https://github.com/ocornut/imgui/issues/5169
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;  // Enable Gamepad Controls

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(m_window, true); // Second param install_callback=true will install GLFW callbacks and chain to existing ones.
    ImGui_ImplOpenGL3_Init();
}

void GeoBox_App::init_gl_viewport() const
{
    int width;
    int height;
    glfwGetWindowSize(m_window, &width, &height);
    glViewport(0, 0, width, height);
}

void GeoBox_App::init_glfw_callbacks()
{
    // Using methods as callbacks: https://stackoverflow.com/a/28660673/8094047
    glfwSetFramebufferSizeCallback(m_window, [](GLFWwindow *w, int width, int height)
                                   { static_cast<GeoBox_App *>(glfwGetWindowUserPointer(w))->framebuffer_size_callback(w, width, height); });
    glfwSetWindowRefreshCallback(m_window, [](GLFWwindow *w)
                                 { static_cast<GeoBox_App *>(glfwGetWindowUserPointer(w))->window_refresh_callback(w); });
}

bool GeoBox_App::init_shaders()
{
    std::optional<std::string> vertex_shader_source = read_file("shaders/default.vert");
    if (!vertex_shader_source.has_value())
    {
        return false;
    }
    std::optional<std::string> fragment_shader_source = read_file("shaders/default.frag");
    if (!fragment_shader_source.has_value())
    {
        return false;
    }

    char *vertex_shader_sources[] = {vertex_shader_source->data()};
    char *fragment_shader_sources[] = {fragment_shader_source->data()};
    int success;
    char info_log[512];

    unsigned int vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex_shader, 1, vertex_shader_sources, nullptr);
    glCompileShader(vertex_shader);
    glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        glGetShaderInfoLog(vertex_shader, 512, nullptr, info_log);
        std::cerr << "ERROR::SHADER::VERTEX::COMPILATION_FAILED\n"
                  << info_log << std::endl;
        return false;
    }

    unsigned int fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader, 1, fragment_shader_sources, nullptr);
    glCompileShader(fragment_shader);
    glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        glGetShaderInfoLog(fragment_shader, 512, nullptr, info_log);
        std::cerr << "ERROR::SHADER::FRAGMENT::COMPILATION_FAILED\n"
                  << info_log << std::endl;
        return false;
    }

    unsigned int shader_program = glCreateProgram();
    glAttachShader(shader_program, vertex_shader);
    glAttachShader(shader_program, fragment_shader);
    glLinkProgram(shader_program);
    glGetProgramiv(shader_program, GL_LINK_STATUS, &success);
    if (!success)
    {
        glGetProgramInfoLog(shader_program, 512, nullptr, info_log);
        std::cerr << "ERROR::SHADER::PROGRAM::LINK_FAILED\n"
                  << info_log << std::endl;
        return false;
    }

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    m_default_shader_program = shader_program;

    return true;
}

void GeoBox_App::window_refresh_callback(GLFWwindow *window)
{
    render();
    glfwSwapBuffers(window);
}

void GeoBox_App::framebuffer_size_callback(const GLFWwindow * /*window*/, int width, int height) const
{
    glViewport(0, 0, width, height);
}

void GeoBox_App::process_input()
{
    // TODO: process input
}

void GeoBox_App::render()
{
    int width;
    int height;
    glfwGetWindowSize(m_window, &width, &height);

    glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(m_default_shader_program);
    float time = glfwGetTime();
    float green_value = (std::sin(time) / 2.0f) + 0.5f;
    int object_color_uniform_location = glGetUniformLocation(m_default_shader_program, "object_color");
    glUniform4f(object_color_uniform_location, 0.0f, green_value, 0.0f, 1.0f);

    Mat4x4f model = Mat4x4f::rotation_axis_angle({0.5f, 1.0f, 0.0f}, time * degrees_to_radians(50.0f));
    Mat4x4f view = Mat4x4f::translation(Vec3f{0.0f, 0.0f, -10.0f});
    Mat4x4f projection = Mat4x4f::perspective(degrees_to_radians(45.0f), (float)width / (float)height, 0.1f, 100.0f);

    int model_matrix_uniform_location = glGetUniformLocation(m_default_shader_program, "model");
    glUniformMatrix4fv(model_matrix_uniform_location, 1, GL_FALSE, model.values_ptr());
    int view_matrix_uniform_location = glGetUniformLocation(m_default_shader_program, "view");
    glUniformMatrix4fv(view_matrix_uniform_location, 1, GL_FALSE, view.values_ptr());
    int projection_matrix_uniform_location = glGetUniformLocation(m_default_shader_program, "projection");
    glUniformMatrix4fv(projection_matrix_uniform_location, 1, GL_FALSE, projection.values_ptr());

    for (const GPU_Mesh &mesh : m_gpu_meshes)
    {
        glBindVertexArray(mesh.m_VAO);
        if (mesh.m_is_indexed)
        {
            glDrawElements(GL_TRIANGLES, mesh.m_num_vertices, GL_UNSIGNED_INT, 0);
        }
        else
        {
            glDrawArrays(GL_TRIANGLES, 0, mesh.m_num_vertices);
        }
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    const ImGuiViewport *main_viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(main_viewport->WorkPos.x, main_viewport->WorkPos.y), ImGuiCond_Once);
    ImGui::SetNextWindowSize(INITIAL_IMGUI_MAIN_WINDOW_SIZE, ImGuiCond_Once);

    ImGui::Begin(IMGUI_MAIN_WINDOW_TITLE, nullptr, ImGuiWindowFlags_None);

    if (ImGui::Button(LOAD_STL_BUTTON_DIALOG_TITLE))
    {
        ImGuiFileDialog::Instance()->OpenDialog(LOAD_STL_DIALOG_KEY, LOAD_STL_BUTTON_DIALOG_TITLE, ".stl", ".");
    }

    if (ImGuiFileDialog::Instance()->Display(LOAD_STL_DIALOG_KEY))
    {
        if (ImGuiFileDialog::Instance()->IsOk())
        {
            std::string file_path = ImGuiFileDialog::Instance()->GetFilePathName();
            on_load_stl_dialog_ok(file_path);
        }
        ImGuiFileDialog::Instance()->Close();
    }

    ImGui::End();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void GeoBox_App::shutdown() const
{
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwTerminate();
}

void GeoBox_App::on_load_stl_dialog_ok(const std::string &file_path)
{
    std::optional<std::vector<Vec3f>> vertices = read_stl_mesh_file(file_path);
    if (!vertices.has_value())
    {
        std::cerr << "Failed to import .stl mesh file: " << file_path << std::endl;
        return;
    }

    unsigned int VAO;
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    unsigned int VBO;
    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, vertices->size() * sizeof(Vec3f), (float *)vertices->data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float[3]), (void *)0);
    glEnableVertexAttribArray(0);

    if (vertices->size() <= std::numeric_limits<unsigned int>::max())
    {
        m_gpu_meshes.emplace_back(VAO, static_cast<unsigned int>(vertices->size()), false);
    }
    else
    {
        std::cerr << "Mesh exceeds maximum number of vertices: " << file_path << std::endl;
        // TODO: maybe we can create multiple VAOs for large meshes?
        // maybe that is why glDrawArrays accepts an offset and a count, really handy,
        // something for the future
    }
}
