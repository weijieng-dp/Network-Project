#pragma once

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "implot.h"
#include "implot_internal.h"
#include "imgui_impl_opengl3.h"
#include "types.h"
#include "utils.h"

#define GLFW_EXPOSE_NATIVE_WGL 
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GL/glew.h> // for access to OpenGL API declarations
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

class Display {
public:
	struct fatStruct;
	static GLFWwindow* window;
public:
	static void Init();
	static void Draw();
	static void Free();
};