#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <iostream>

// Window size
const int WIDTH = 1280;
const int HEIGHT = 720;

int main()
{
    // ==========================================
    // Init GLFW
    // ==========================================
    if (!glfwInit())
    {
        std::cerr << "Failed to initialize GLFW\n";
        return -1;
    }

    // Set OpenGL version (important for GLEW + modern GL)
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    // ==========================================
    // Create Window
    // ==========================================
    GLFWwindow* window = glfwCreateWindow(WIDTH, HEIGHT, "Client", nullptr, nullptr);
    if (!window)
    {
        std::cerr << "Failed to create GLFW window\n";
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(window);

    // ==========================================
    // Init GLEW
    // ==========================================
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK)
    {
        std::cerr << "Failed to initialize GLEW\n";
        return -1;
    }

    // Fix for GLEW bug (optional but common)
    glGetError();

    // ==========================================
    // Viewport
    // ==========================================
    glViewport(0, 0, WIDTH, HEIGHT);

    // ==========================================
    // Main Loop
    // ==========================================
    while (!glfwWindowShouldClose(window))
    {
        // Input
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
            glfwSetWindowShouldClose(window, true);

        // Render
        glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Swap + Poll
        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    // ==========================================
    // Cleanup
    // ==========================================
    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}