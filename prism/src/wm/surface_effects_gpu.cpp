#include "surface_effects_internal.hpp"

namespace prism::wm {
std::unique_ptr<Buffer> SurfaceEffects::Impl::Allocate(int width, int height)
{
    ++counters.allocation_attempts;
    auto fail = [this]() {
        ++counters.allocation_failures;
        return std::unique_ptr<Buffer>{};
    };

    auto *formats = wlr_renderer_get_texture_formats(renderer, allocator->buffer_caps);
    auto *fmt = wlr_drm_format_set_get(formats, DRM_FORMAT_ARGB8888);
    if (!fmt) {
        return fail();
    }
    auto result = std::make_unique<Buffer>();
    result->buffer = wlr_allocator_create_buffer(allocator, width, height, fmt);
    if (!result->buffer) {
        return fail();
    }

    // Initialize imported render target before creating its sampling texture.
    auto *pass = wlr_renderer_begin_buffer_pass(renderer, result->buffer, nullptr);
    if (!pass) {
        return fail();
    }
    wlr_render_rect_options clear{};
    clear.box = {0, 0, width, height};
    clear.blend_mode = WLR_RENDER_BLEND_MODE_NONE;
    wlr_render_pass_add_rect(pass, &clear);
    if (!wlr_render_pass_submit(pass)) {
        return fail();
    }

    result->texture = wlr_texture_from_buffer(renderer, result->buffer);
    if (!result->texture) {
        return fail();
    }
    ++counters.allocated_buffers;
    return result;
}

bool SurfaceEffects::Impl::Draw(Buffer &target, Buffer &source, GLuint program, int width,
                                int height, const Region *region, double padding,
                                const contracts::ThemeDecoration *decoration)
{
    ++counters.material_pass_attempts;
    auto *pass = wlr_renderer_begin_buffer_pass(renderer, target.buffer, nullptr);
    if (!pass) {
        return false;
    }
    wlr_gles2_texture_attribs attrib{};
    wlr_gles2_texture_get_attribs(source.texture, &attrib);
    if (attrib.target != GL_TEXTURE_2D) {
        wlr_render_pass_submit(pass);
        return false;
    }

    glUseProgram(program);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, attrib.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glUniform1i(glGetUniformLocation(program, "image"), 0);
    if (region) {
        glUniform2f(glGetUniformLocation(program, "size"), width, height);
        glUniform4f(glGetUniformLocation(program, "box"), padding, padding, region->bounds.width,
                    region->bounds.height);
        glUniform1f(
            glGetUniformLocation(program, "radius"),
            std::min({region->corner_radius, region->bounds.width / 2, region->bounds.height / 2}));
        glUniform1f(glGetUniformLocation(program, "shadow"),
                    decoration ? decoration->shadow_blur : 0);
        glUniform1f(glGetUniformLocation(program, "blur_enabled"), region->blur_radius > 0 ? 1 : 0);
        const auto border = decoration ? decoration->border : contracts::Color{};
        glUniform1f(glGetUniformLocation(program, "border_width"),
                    decoration ? decoration->border_width : 0);
        glUniform4f(glGetUniformLocation(program, "border_color"), border.r / 255.f,
                    border.g / 255.f, border.b / 255.f, decoration ? border.a / 255.f : 0);
        const auto shade = decoration ? decoration->shadow : contracts::Color{};
        glUniform4f(glGetUniformLocation(program, "shadow_color"), shade.r / 255.f, shade.g / 255.f,
                    shade.b / 255.f, shade.a / 255.f);
        glUniform1f(glGetUniformLocation(program, "shadow_offset"),
                    decoration ? decoration->shadow_y : 0);
    }

    static const GLfloat quad[]{0, 0, 1, 0, 0, 1, 1, 1};
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);

    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);

    bool ok = glGetError() == GL_NO_ERROR;
    const bool submitted = wlr_render_pass_submit(pass) && ok;
    if (submitted) {
        ++counters.material_passes;
    }
    return submitted;
}

bool SurfaceEffects::Impl::Blur(Buffer &target, Buffer &source, double radius, bool horizontal)
{
    ++counters.blur_pass_attempts;
    auto *pass = wlr_renderer_begin_buffer_pass(renderer, target.buffer, nullptr);
    if (!pass) {
        return false;
    }
    wlr_gles2_texture_attribs attr{};
    wlr_gles2_texture_get_attribs(source.texture, &attr);
    if (attr.target != GL_TEXTURE_2D) {
        wlr_render_pass_submit(pass);
        return false;
    }

    glUseProgram(blur);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, attr.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glUniform1i(glGetUniformLocation(blur, "image"), 0);
    // Two separable passes at half resolution; sample footprint is logical.
    glUniform2f(glGetUniformLocation(blur, "step_size"),
                horizontal ? radius / (6.46 * source.buffer->width) : 0,
                horizontal ? 0 : radius / (6.46 * source.buffer->height));

    static const GLfloat quad[]{0, 0, 1, 0, 0, 1, 1, 1};
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);

    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);

    bool ok = glGetError() == GL_NO_ERROR;
    const bool submitted = wlr_render_pass_submit(pass) && ok;
    if (submitted) {
        ++counters.blur_passes;
    }
    return submitted;
}

} // namespace prism::wm
