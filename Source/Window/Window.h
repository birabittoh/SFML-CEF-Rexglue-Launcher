#pragma once
#ifndef __gl_h_
#include "glad/glad.h"
#endif
#include "stb_image/stb_image.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#define GL_SILENCE_DEPRECATION

#include <GLFW/glfw3.h>
#include <memory>


class VinceWindow {
	public:
		VinceWindow(int width, int height, const char* title)
			: width(width),
			height(height),
			title(title),
			window(nullptr, glfwDestroyWindow)
		{
			init();
		}
	void init();
	void EnableBlur();

	void EndFrame() {
		glfwSwapBuffers(window.get());
	}
	GLFWwindow* getWindow() const {
		return window.get();
	}
	const char* glsl_version;

	int width, height;
private:
		const char* title;
		std::unique_ptr<GLFWwindow, void(*)(GLFWwindow*)> window; // Use custom deleter type
};

