namespace ui_draw { 

//- Frame entry point
void frame()
{
    // Start the Dear ImGui frame
    ImGui_ImplOpenGL2_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // Drawing 

    // Rendering
    ImGui::Render();
    int display_w, display_h;
    glfwGetFramebufferSize(g_ui_state.window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
    glfwMakeContextCurrent(g_ui_state.window);
    glfwSwapBuffers(g_ui_state.window);
}

} // namespace ui_draw