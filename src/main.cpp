#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <pty.h>


#include "core.h"
#include "core.c"

#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#endif
#include <GLFW/glfw3.h>
#include <imgui-all.hpp>

#include "gdb.cpp"
#include "ui_core.cpp"
#include "ui_draw.cpp"

int main(int argc, char** argv)
{
    gdb::init();

    i32 err = ui_core::init();
    if (err != 0)
        return 1;

    // Main loop
    while (!glfwWindowShouldClose(g_ui_state.window))
    {
        // Poll and handle events (inputs, window resize, etc.)
        // You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
        // - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
        // - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
        // Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
        glfwPollEvents();
        if (glfwGetWindowAttrib(g_ui_state.window, GLFW_ICONIFIED) != 0)
        {
            ImGui_ImplGlfw_Sleep(10);
            continue;
        }

        ui_draw::frame();
    }

    ui_core::destroy();
    gdb::destroy();

    return 0;
}
