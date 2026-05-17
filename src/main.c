/*
 * Fugazi — CRT Shader Tweaker with Live Preview
 * Apostrophe UI + PakKit + OpenGL ES 2.0 shader preview.
 *
 * Layout:
 *   Full-screen game screenshot rendered through Fugazi shader passes
 *   Semi-transparent bottom bar: current parameter + button hints
 *
 * Controls:
 *   Up/Down: cycle params
 *   Left/Right: adjust value
 *   A: install shader to system
 *   Y: clear all effects (passthrough)
 *   B: exit
 *
 * On launch, loads the currently installed shader settings.
 * "Install" writes updated values to system Shaders folder + clears cache.
 */

#define AP_IMPLEMENTATION
#include "apostrophe.h"

#define PAKKIT_UI_IMPLEMENTATION
#include "pakkit_ui.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

#ifdef PLATFORM_TG5040
#include <GLES2/gl2.h>
#include <EGL/egl.h>
#else
/* Desktop stub — preview won't render but app compiles */
#include <SDL2/SDL_opengl.h>
#endif

/* -----------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------- */

#define MAX_PATH_LEN     1280

/* Parameter limits */
#define MAX_PARAMS       8

/* -----------------------------------------------------------------------
 * Shader parameter definition
 * ----------------------------------------------------------------------- */

typedef struct {
    const char *name;        /* Matches #define name in GLSL */
    const char *label;       /* Display label */
    const char *description; /* Short explanation */
    float       value;       /* Current value */
    float       clear_val;   /* "Clear" value (no visible effect) */
    float       min;
    float       max;
    float       step;
    int         shader;      /* 0 = glow shader, 1 = scanline shader */
} fugazi_param;

/* -----------------------------------------------------------------------
 * App state
 * ----------------------------------------------------------------------- */

typedef struct {
    fugazi_param params[MAX_PARAMS];
    int          param_count;
    int          cursor;        /* Current param index */

    /* Paths */
    char         pak_dir[MAX_PATH_LEN];
    char         pak_glow_path[MAX_PATH_LEN];    /* pak defaults */
    char         pak_scanline_path[MAX_PATH_LEN]; /* pak defaults */
    char         cfg_path[MAX_PATH_LEN];          /* pak's cfg template */

    char         sys_shader_dir[MAX_PATH_LEN];
    char         sys_glow_path[MAX_PATH_LEN];
    char         sys_scanline_path[MAX_PATH_LEN];
    char         sys_cfg_path[MAX_PATH_LEN];

    /* Preview */
    SDL_Texture *preview_texture;
    GLuint       gl_program_glow;
    GLuint       gl_program_scan;
    GLuint       gl_source_tex;
    GLuint       gl_fbo;
    GLuint       gl_fbo_tex;
    GLuint       gl_output_tex;
    int          preview_w;
    int          preview_h;
    int          source_w;
    int          source_h;
    int          sim_input_w;
    int          sim_input_h;
    int          gl_initialized;
} fugazi_state;

static fugazi_state state;

/* -----------------------------------------------------------------------
 * Parameter definitions
 * ----------------------------------------------------------------------- */

static void init_params(void)
{
    int i = 0;

    state.params[i++] = (fugazi_param){
        .name = "CURVATURE", .label = "Curvature",
        .description = "Screen edge bend",
        .value = 0.06f, .clear_val = 0.0f,
        .min = 0.0f, .max = 0.25f, .step = 0.01f, .shader = 0
    };
    state.params[i++] = (fugazi_param){
        .name = "GLOW_MIX", .label = "Glow",
        .description = "Soft light bleed",
        .value = 0.35f, .clear_val = 0.0f,
        .min = 0.0f, .max = 0.8f, .step = 0.05f, .shader = 0
    };
    state.params[i++] = (fugazi_param){
        .name = "SCANLINE_WEIGHT", .label = "Scanlines",
        .description = "Dark line strength",
        .value = 0.85f, .clear_val = 0.0f,
        .min = 0.0f, .max = 1.0f, .step = 0.05f, .shader = 1
    };
    state.params[i++] = (fugazi_param){
        .name = "SCANLINE_GAP", .label = "Gap Darkness",
        .description = "How dark between lines",
        .value = 0.10f, .clear_val = 0.0f,
        .min = 0.0f, .max = 0.5f, .step = 0.05f, .shader = 1
    };
    state.params[i++] = (fugazi_param){
        .name = "MASK_STRENGTH", .label = "Phosphor Mask",
        .description = "Vertical color stripes",
        .value = 0.35f, .clear_val = 0.0f,
        .min = 0.0f, .max = 0.6f, .step = 0.05f, .shader = 1
    };
    state.params[i++] = (fugazi_param){
        .name = "VIGNETTE", .label = "Vignette",
        .description = "Dark corners",
        .value = 0.25f, .clear_val = 0.0f,
        .min = 0.0f, .max = 0.7f, .step = 0.05f, .shader = 1
    };
    state.params[i++] = (fugazi_param){
        .name = "BRIGHTNESS", .label = "Brightness",
        .description = "Compensate for darkening",
        .value = 1.45f, .clear_val = 1.0f,
        .min = 0.5f, .max = 2.5f, .step = 0.05f, .shader = 1
    };
    state.params[i++] = (fugazi_param){
        .name = "WARMTH", .label = "Warmth",
        .description = "Warm color shift",
        .value = 0.06f, .clear_val = 0.0f,
        .min = 0.0f, .max = 0.30f, .step = 0.01f, .shader = 1
    };

    state.param_count = i;
}

/* -----------------------------------------------------------------------
 * Clear — reset all effects to passthrough
 * ----------------------------------------------------------------------- */

static void clear_all_params(void)
{
    for (int i = 0; i < state.param_count; i++) {
        state.params[i].value = state.params[i].clear_val;
    }
}

/* -----------------------------------------------------------------------
 * File I/O — Read current values from shader files
 * ----------------------------------------------------------------------- */

static void load_params_from_file(const char *path, int shader_id)
{
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "#define ", 8) != 0) continue;

        for (int i = 0; i < state.param_count; i++) {
            if (state.params[i].shader != shader_id) continue;

            char search[128];
            snprintf(search, sizeof(search), "#define %s ",
                     state.params[i].name);
            if (strncmp(line, search, strlen(search)) == 0) {
                /* Value is everything after "#define NAME " */
                float val = 0.0f;
                sscanf(line + strlen(search), "%f", &val);
                state.params[i].value = val;
                break;
            }
        }
    }
    fclose(f);
}

/* -----------------------------------------------------------------------
 * File I/O — Write updated values to shader files
 * ----------------------------------------------------------------------- */

static void save_param_to_file(const char *path, fugazi_param *param)
{
    FILE *f = fopen(path, "r");
    if (!f) return;

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return; }
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);

    /* Find and replace the #define line */
    char search[128];
    snprintf(search, sizeof(search), "#define %s ", param->name);

    char *pos = strstr(buf, search);
    if (!pos) { free(buf); return; }

    /* Find end of this line */
    char *eol = strchr(pos, '\n');
    if (!eol) eol = buf + len;

    /* Build replacement line */
    char new_line[128];
    snprintf(new_line, sizeof(new_line), "#define %s %.2f", param->name, param->value);

    /* Write new file */
    f = fopen(path, "w");
    if (!f) { free(buf); return; }
    fwrite(buf, 1, pos - buf, f);
    fputs(new_line, f);
    fwrite(eol, 1, (buf + len) - eol, f);
    fclose(f);
    free(buf);
}

/* -----------------------------------------------------------------------
 * File I/O — Copy file helper
 * ----------------------------------------------------------------------- */

static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) return -1;

    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            return -1;
        }
    }

    fclose(in);
    fclose(out);
    return 0;
}

/* -----------------------------------------------------------------------
 * Apply — Install shaders to system Shaders folder
 * ----------------------------------------------------------------------- */

static void apply_to_system(void)
{
    /* Ensure target directories exist */
    mkdir(state.sys_shader_dir, 0755);

    char glsl_dir[MAX_PATH_LEN + 8];
    snprintf(glsl_dir, sizeof(glsl_dir), "%s/glsl", state.sys_shader_dir);
    mkdir(glsl_dir, 0755);

    /* Copy pak shader templates to system, then update #define values */
    int ok = 0;
    ok |= copy_file(state.pak_glow_path, state.sys_glow_path);
    ok |= copy_file(state.pak_scanline_path, state.sys_scanline_path);
    ok |= copy_file(state.cfg_path, state.sys_cfg_path);

    if (ok != 0) {
        pakkit_message("Failed to install shader.\nCheck SD card permissions.", "OK");
        return;
    }

    /* Write current param values into system shader files */
    for (int i = 0; i < state.param_count; i++) {
        const char *path = (state.params[i].shader == 0)
            ? state.sys_glow_path : state.sys_scanline_path;
        save_param_to_file(path, &state.params[i]);
    }

    /* Clear shader cache so minarch recompiles from updated sources */
    unlink("/mnt/SDCARD/.shadercache/fugazi-glow.glsl.bin");
    unlink("/mnt/SDCARD/.shadercache/fugazi-scanline.glsl.bin");
}

/* -----------------------------------------------------------------------
 * OpenGL ES Preview — Shader compilation
 * ----------------------------------------------------------------------- */

static GLuint compile_shader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        ap_log("Shader compile error: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint link_program(GLuint vert, GLuint frag)
{
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);
    glLinkProgram(prog);

    GLint ok;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        ap_log("Program link error: %s", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

/* -----------------------------------------------------------------------
 * Preview vertex shader (shared by both passes)
 * ----------------------------------------------------------------------- */

static const char *preview_vert_src =
    "#version 100\n"
    "precision mediump float;\n"
    "attribute vec4 a_position;\n"
    "attribute vec2 a_texcoord;\n"
    "varying vec2 TEX0;\n"
    "uniform vec2 TextureSize;\n"
    "varying vec2 v_offset_x;\n"
    "varying vec2 v_offset_y;\n"
    "void main() {\n"
    "    gl_Position = a_position;\n"
    "    TEX0 = a_texcoord;\n"
    "    v_offset_x = vec2(2.0 / TextureSize.x, 0.0);\n"
    "    v_offset_y = vec2(0.0, 2.0 / TextureSize.y);\n"
    "}\n";

/* -----------------------------------------------------------------------
 * Preview fragment shaders — embedded versions of fugazi shaders
 * with uniforms for live parameter adjustment
 * ----------------------------------------------------------------------- */

static const char *preview_glow_frag_src =
    "#version 100\n"
    "precision mediump float;\n"
    "uniform sampler2D Texture;\n"
    "uniform vec2 InputSize;\n"
    "uniform vec2 TextureSize;\n"
    "uniform float CURVATURE;\n"
    "uniform float GLOW_MIX;\n"
    "varying vec2 TEX0;\n"
    "varying vec2 v_offset_x;\n"
    "varying vec2 v_offset_y;\n"
    "void main() {\n"
    "    vec2 scale = TextureSize / InputSize;\n"
    "    vec2 tex0 = TEX0 * scale;\n"
    "    vec2 centered = tex0 - vec2(0.5);\n"
    "    float r2 = dot(centered, centered);\n"
    "    centered *= 1.0 + CURVATURE * r2;\n"
    "    vec2 edge = step(vec2(-0.5), centered) * step(centered, vec2(0.5));\n"
    "    float inside = edge.x * edge.y;\n"
    "    vec2 uv = (centered + vec2(0.5)) / scale;\n"
    "    vec3 center = texture2D(Texture, uv).rgb;\n"
    "    vec3 blur = texture2D(Texture, uv + v_offset_x).rgb;\n"
    "    blur += texture2D(Texture, uv - v_offset_x).rgb;\n"
    "    blur += texture2D(Texture, uv + v_offset_y).rgb;\n"
    "    blur += texture2D(Texture, uv - v_offset_y).rgb;\n"
    "    blur *= 0.25;\n"
    "    vec3 color = mix(center, blur, GLOW_MIX);\n"
    "    gl_FragColor = vec4(color * inside, 1.0);\n"
    "}\n";

static const char *preview_scan_frag_src =
    "#version 100\n"
    "precision mediump float;\n"
    "uniform sampler2D Texture;\n"
    "uniform vec2 InputSize;\n"
    "uniform vec2 TextureSize;\n"
    "uniform vec2 OutputSize;\n"
    "uniform float SCANLINE_WEIGHT;\n"
    "uniform float SCANLINE_GAP;\n"
    "uniform float MASK_STRENGTH;\n"
    "uniform float VIGNETTE;\n"
    "uniform float BRIGHTNESS;\n"
    "uniform float WARMTH;\n"
    "varying vec2 TEX0;\n"
    "void main() {\n"
    "    vec3 color = texture2D(Texture, TEX0).rgb;\n"
    "    float luma = dot(color, vec3(0.299, 0.587, 0.114));\n"
    "    float scanline_pos = TEX0.y * InputSize.y;\n"
    "    float frac_y = fract(scanline_pos);\n"
    "    float dist = abs(frac_y - 0.5) * 2.0;\n"
    "    float adaptive_weight = SCANLINE_WEIGHT * (1.0 - luma * 0.5);\n"
    "    float scanline = 1.0 - dist * dist * adaptive_weight;\n"
    "    scanline = max(scanline, SCANLINE_GAP);\n"
    "    color *= scanline;\n"
    "    float pixel_x = floor(TEX0.x * OutputSize.x);\n"
    "    float phase = mod(pixel_x, 3.0);\n"
    "    float is0 = 1.0 - step(0.5, phase);\n"
    "    float is1 = step(0.5, phase) * (1.0 - step(1.5, phase));\n"
    "    float is2 = step(1.5, phase);\n"
    "    float mask_fade = smoothstep(0.05, 0.25, luma);\n"
    "    float effective_mask = MASK_STRENGTH * mask_fade;\n"
    "    float dim = 1.0 - effective_mask;\n"
    "    vec3 mask;\n"
    "    mask.r = is0 + (1.0 - is0) * dim;\n"
    "    mask.g = is1 + (1.0 - is1) * dim;\n"
    "    mask.b = is2 + (1.0 - is2) * dim;\n"
    "    color *= mask;\n"
    "    color.r *= 1.0 + WARMTH;\n"
    "    color.g *= 1.0 + WARMTH * 0.4;\n"
    "    color.b *= 1.0 - WARMTH * 0.6;\n"
    "    color *= BRIGHTNESS;\n"
    "    vec2 vig_uv = TEX0 * 2.0 - 1.0;\n"
    "    float vig = 1.0 - dot(vig_uv, vig_uv) * VIGNETTE;\n"
    "    color *= vig;\n"
    "    color = min(color, vec3(1.0));\n"
    "    gl_FragColor = vec4(color, 1.0);\n"
    "}\n";

/* -----------------------------------------------------------------------
 * Preview rendering
 * ----------------------------------------------------------------------- */

static const float quad_verts[] = {
    /* position (x,y), texcoord (u,v) — v flipped for GL readback */
    -1.0f, -1.0f,  0.0f, 0.0f,
     1.0f, -1.0f,  1.0f, 0.0f,
    -1.0f,  1.0f,  0.0f, 1.0f,
     1.0f,  1.0f,  1.0f, 1.0f,
};

static void set_uniform_float(GLuint prog, const char *name, float val)
{
    GLint loc = glGetUniformLocation(prog, name);
    if (loc >= 0) glUniform1f(loc, val);
}

static void set_uniform_vec2(GLuint prog, const char *name, float x, float y)
{
    GLint loc = glGetUniformLocation(prog, name);
    if (loc >= 0) glUniform2f(loc, x, y);
}

static float get_param_value(const char *name)
{
    for (int i = 0; i < state.param_count; i++) {
        if (strcmp(state.params[i].name, name) == 0)
            return state.params[i].value;
    }
    return 0.0f;
}

static int init_gl_preview(SDL_Window *window, const char *image_path)
{
    (void)window;

    /* Load source image */
    SDL_Surface *img = IMG_Load(image_path);
    if (!img) {
        ap_log("Failed to load preview image: %s", image_path);
        return -1;
    }

    /* Convert to RGBA */
    SDL_Surface *rgba = SDL_ConvertSurfaceFormat(img, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(img);
    if (!rgba) return -1;

    state.source_w = rgba->w;
    state.source_h = rgba->h;
    state.preview_w = rgba->w;
    state.preview_h = rgba->h;

    /* Simulated retro input resolution — this is what the emulator would
     * output before upscaling. Scanlines/mask depend on this being low-res.
     * 320x224 = typical Genesis/Mega Drive resolution */
    state.sim_input_w = 320;
    state.sim_input_h = 224;

    /* Create source texture */
    glGenTextures(1, &state.gl_source_tex);
    glBindTexture(GL_TEXTURE_2D, state.gl_source_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);

    /* Create FBO + intermediate texture for pass 1 output */
    glGenTextures(1, &state.gl_fbo_tex);
    glBindTexture(GL_TEXTURE_2D, state.gl_fbo_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    glGenFramebuffers(1, &state.gl_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, state.gl_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, state.gl_fbo_tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    /* Create output texture */
    glGenTextures(1, &state.gl_output_tex);
    glBindTexture(GL_TEXTURE_2D, state.gl_output_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    SDL_FreeSurface(rgba);

    /* Compile shaders */
    GLuint vert = compile_shader(GL_VERTEX_SHADER, preview_vert_src);
    if (!vert) return -1;

    GLuint frag_glow = compile_shader(GL_FRAGMENT_SHADER, preview_glow_frag_src);
    if (!frag_glow) return -1;

    GLuint frag_scan = compile_shader(GL_FRAGMENT_SHADER, preview_scan_frag_src);
    if (!frag_scan) return -1;

    state.gl_program_glow = link_program(vert, frag_glow);
    state.gl_program_scan = link_program(vert, frag_scan);

    glDeleteShader(vert);
    glDeleteShader(frag_glow);
    glDeleteShader(frag_scan);

    if (!state.gl_program_glow || !state.gl_program_scan) return -1;

    state.gl_initialized = 1;
    return 0;
}

static void render_preview(void)
{
    if (!state.gl_initialized) return;

    float tex_w = (float)state.source_w;
    float tex_h = (float)state.source_h;
    float input_w = (float)state.sim_input_w;
    float input_h = (float)state.sim_input_h;

    /* --- Pass 1: Glow (render to FBO) --- */
    glBindFramebuffer(GL_FRAMEBUFFER, state.gl_fbo);
    glViewport(0, 0, state.preview_w, state.preview_h);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(state.gl_program_glow);

    GLint pos_loc = glGetAttribLocation(state.gl_program_glow, "a_position");
    GLint tex_loc = glGetAttribLocation(state.gl_program_glow, "a_texcoord");

    glVertexAttribPointer(pos_loc, 2, GL_FLOAT, GL_FALSE, 16, quad_verts);
    glEnableVertexAttribArray(pos_loc);
    glVertexAttribPointer(tex_loc, 2, GL_FLOAT, GL_FALSE, 16, quad_verts + 2);
    glEnableVertexAttribArray(tex_loc);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, state.gl_source_tex);
    glUniform1i(glGetUniformLocation(state.gl_program_glow, "Texture"), 0);

    set_uniform_vec2(state.gl_program_glow, "InputSize", tex_w, tex_h);
    set_uniform_vec2(state.gl_program_glow, "TextureSize", tex_w, tex_h);
    set_uniform_float(state.gl_program_glow, "CURVATURE",
                      get_param_value("CURVATURE"));
    set_uniform_float(state.gl_program_glow, "GLOW_MIX",
                      get_param_value("GLOW_MIX"));

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    /* --- Pass 2: Scanlines (render to output texture via FBO swap) --- */
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, state.gl_output_tex, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(state.gl_program_scan);

    pos_loc = glGetAttribLocation(state.gl_program_scan, "a_position");
    tex_loc = glGetAttribLocation(state.gl_program_scan, "a_texcoord");

    glVertexAttribPointer(pos_loc, 2, GL_FLOAT, GL_FALSE, 16, quad_verts);
    glEnableVertexAttribArray(pos_loc);
    glVertexAttribPointer(tex_loc, 2, GL_FLOAT, GL_FALSE, 16, quad_verts + 2);
    glEnableVertexAttribArray(tex_loc);

    glBindTexture(GL_TEXTURE_2D, state.gl_fbo_tex);
    glUniform1i(glGetUniformLocation(state.gl_program_scan, "Texture"), 0);

    set_uniform_vec2(state.gl_program_scan, "InputSize", input_w, input_h);
    set_uniform_vec2(state.gl_program_scan, "TextureSize", tex_w, tex_h);
    set_uniform_vec2(state.gl_program_scan, "OutputSize",
                     (float)state.preview_w, (float)state.preview_h);
    set_uniform_float(state.gl_program_scan, "SCANLINE_WEIGHT",
                      get_param_value("SCANLINE_WEIGHT"));
    set_uniform_float(state.gl_program_scan, "SCANLINE_GAP",
                      get_param_value("SCANLINE_GAP"));
    set_uniform_float(state.gl_program_scan, "MASK_STRENGTH",
                      get_param_value("MASK_STRENGTH"));
    set_uniform_float(state.gl_program_scan, "VIGNETTE",
                      get_param_value("VIGNETTE"));
    set_uniform_float(state.gl_program_scan, "BRIGHTNESS",
                      get_param_value("BRIGHTNESS"));
    set_uniform_float(state.gl_program_scan, "WARMTH",
                      get_param_value("WARMTH"));

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    /* Restore default framebuffer */
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, state.gl_fbo_tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static SDL_Texture *get_preview_as_sdl_texture(SDL_Renderer *renderer)
{
    if (!state.gl_initialized) return NULL;

    /* Read pixels from output texture */
    int w = state.preview_w;
    int h = state.preview_h;
    unsigned char *pixels = malloc(w * h * 4);
    if (!pixels) return NULL;

    glBindFramebuffer(GL_FRAMEBUFFER, state.gl_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, state.gl_output_tex, 0);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, state.gl_fbo_tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    int stride = w * 4;

    /* Create/update SDL texture */
    if (state.preview_texture) {
        SDL_UpdateTexture(state.preview_texture, NULL, pixels, stride);
    } else {
        state.preview_texture = SDL_CreateTexture(renderer,
            SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        if (state.preview_texture) {
            SDL_UpdateTexture(state.preview_texture, NULL, pixels, stride);
        }
    }

    free(pixels);
    return state.preview_texture;
}

/* -----------------------------------------------------------------------
 * Main
 * ----------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    memset(&state, 0, sizeof(state));

    /* Resolve paths */
    const char *pak_dir = getenv("FUGAZI_PAK_DIR");
    if (!pak_dir) pak_dir = ".";

    snprintf(state.pak_dir, MAX_PATH_LEN, "%s", pak_dir);

    /* Pak shader templates (defaults) */
    snprintf(state.pak_glow_path, MAX_PATH_LEN,
             "%s/shaders/fugazi-glow.glsl", pak_dir);
    snprintf(state.pak_scanline_path, MAX_PATH_LEN,
             "%s/shaders/fugazi-scanline.glsl", pak_dir);
    snprintf(state.cfg_path, MAX_PATH_LEN, "%s/shaders/fugazi.cfg", pak_dir);

    /* System install targets */
    snprintf(state.sys_shader_dir, MAX_PATH_LEN, "/mnt/SDCARD/Shaders");
    snprintf(state.sys_glow_path, MAX_PATH_LEN,
             "/mnt/SDCARD/Shaders/glsl/fugazi-glow.glsl");
    snprintf(state.sys_scanline_path, MAX_PATH_LEN,
             "/mnt/SDCARD/Shaders/glsl/fugazi-scanline.glsl");
    snprintf(state.sys_cfg_path, MAX_PATH_LEN,
             "/mnt/SDCARD/Shaders/fugazi.cfg");

    /* Initialize parameters with defaults */
    init_params();

    /* Load currently installed settings (system files).
     * If not yet installed, keep the init_params defaults. */
    if (access(state.sys_glow_path, F_OK) == 0) {
        load_params_from_file(state.sys_glow_path, 0);
        load_params_from_file(state.sys_scanline_path, 1);
    }

    /* Initialize Apostrophe */
    ap_config config = {
        .window_title = "Fugazi",
        .cpu_speed = AP_CPU_SPEED_NORMAL,
    };

    if (ap_init(&config) != AP_OK) {
        fprintf(stderr, "Failed to initialize Apostrophe\n");
        return 1;
    }

    /* Splash screen */
    {
        char splash_path[MAX_PATH_LEN];
        snprintf(splash_path, sizeof(splash_path), "%s/res/splash.png", pak_dir);

        SDL_Texture *splash = ap_load_image(splash_path);
        if (splash) {
            SDL_Renderer *rend = ap_get_renderer();
            int sw = ap_get_screen_width();
            int sh = ap_get_screen_height();
            int img_w, img_h;
            SDL_QueryTexture(splash, NULL, NULL, &img_w, &img_h);

            float scale_w = (float)sw / (float)img_w;
            float scale_h = (float)sh / (float)img_h;
            float scale = (scale_w < scale_h) ? scale_w : scale_h;
            int draw_w = (int)(img_w * scale);
            int draw_h = (int)(img_h * scale);
            int x = (sw - draw_w) / 2;
            int y = (sh - draw_h) / 2;

            ap_clear_screen();
            SDL_SetRenderDrawColor(rend, 0x06, 0x00, 0x0f, 0xFF);
            SDL_Rect full = {0, 0, sw, sh};
            SDL_RenderFillRect(rend, &full);
            ap_draw_image(splash, x, y, draw_w, draw_h);
            ap_present();

            int waited = 0;
            while (waited < 900) {
                ap_input_event ev;
                while (ap_poll_input(&ev)) {
                    if (ev.pressed && !ev.repeated) waited = 900;
                }
                SDL_Delay(16);
                waited += 16;
            }
            SDL_DestroyTexture(splash);
        }
    }

    /* Initialize GL preview */
    char preview_path[MAX_PATH_LEN];
    snprintf(preview_path, MAX_PATH_LEN, "%s/res/preview.png", pak_dir);

    SDL_Window *window = ap_get_window();
    SDL_Renderer *renderer = ap_get_renderer();

    int gl_ok = init_gl_preview(window, preview_path);
    if (gl_ok != 0) {
        ap_log("GL preview init failed - running without preview");
    }

    /* Main loop */
    int running = 1;
    state.cursor = 0;

    while (running) {
        ap_input_event ev;
        while (ap_poll_input(&ev)) {
            if (!ev.pressed) continue;

            switch (ev.button) {
                case AP_BTN_B:
                    if (!ev.repeated) running = 0;
                    break;

                case AP_BTN_UP:
                    state.cursor--;
                    if (state.cursor < 0)
                        state.cursor = state.param_count - 1;
                    break;

                case AP_BTN_DOWN:
                    state.cursor++;
                    if (state.cursor >= state.param_count)
                        state.cursor = 0;
                    break;

                case AP_BTN_LEFT: {
                    fugazi_param *p = &state.params[state.cursor];
                    p->value -= p->step;
                    if (p->value < p->min) p->value = p->min;
                    break;
                }

                case AP_BTN_RIGHT: {
                    fugazi_param *p = &state.params[state.cursor];
                    p->value += p->step;
                    if (p->value > p->max) p->value = p->max;
                    break;
                }

                case AP_BTN_A:
                    if (!ev.repeated) {
                        int confirm = pakkit_confirm(
                            "Install Fugazi Shader?",
                            "Install", "Cancel");
                        if (confirm == AP_OK) {
                            apply_to_system();
                        }
                    }
                    break;

                case AP_BTN_Y:
                    if (!ev.repeated) {
                        clear_all_params();
                    }
                    break;

                default:
                    break;
            }
        }

        /* --- Render --- */

        /* Render shader preview */
        if (state.gl_initialized) {
            render_preview();
        }

        ap_clear_screen();

        int sw = ap_get_screen_width();
        int sh = ap_get_screen_height();
        int pad = AP_DS(5);

        TTF_Font *font_small = ap_get_font(AP_FONT_SMALL);
        TTF_Font *font_tiny = ap_get_font(AP_FONT_TINY);
        ap_theme *theme = ap_get_theme();

        /* Measure the bottom bar height: param line + hint line + padding */
        int param_line_h = TTF_FontHeight(font_small);
        int hint_line_h = TTF_FontHeight(font_tiny);
        int bar_h = param_line_h + hint_line_h + pad * 4;

        /* --- Full-screen preview --- */
        if (state.gl_initialized) {
            SDL_Texture *tex = get_preview_as_sdl_texture(renderer);
            if (tex) {
                /* Fill screen, leaving room for the bar */
                int avail_h = sh - bar_h;
                float aspect = (float)state.preview_w / (float)state.preview_h;
                int dst_w = sw;
                int dst_h = (int)(dst_w / aspect);
                if (dst_h > avail_h) {
                    dst_h = avail_h;
                    dst_w = (int)(dst_h * aspect);
                }
                int dst_x = (sw - dst_w) / 2;
                int dst_y = (avail_h - dst_h) / 2;

                SDL_Rect dst = { dst_x, dst_y, dst_w, dst_h };
                SDL_RenderCopy(renderer, tex, NULL, &dst);
            }
        } else {
            ap_color hint = theme->hint;
            const char *msg = "Preview not available";
            int msg_w = ap_measure_text(font_small, msg);
            ap_draw_text(font_small, msg, (sw - msg_w) / 2, sh / 2, hint);
        }

        /* --- Bottom bar: semi-transparent background --- */
        int bar_y = sh - bar_h;
        ap_color bar_bg = { 0, 0, 0, 180 };
        ap_draw_rect(0, bar_y, sw, bar_h, bar_bg);

        /* --- Parameter line --- */
        {
            fugazi_param *p = &state.params[state.cursor];
            int py = bar_y + pad;

            /* Value string */
            char val_str[32];
            snprintf(val_str, sizeof(val_str), "%.2f", p->value);

            /* Draw: < Label          value > */
            ap_color text_color = theme->text;
            ap_color hl_color = theme->highlight;

            /* Left arrow + "Label — description" */
            int arrow_w = ap_measure_text(font_small, "<");
            ap_draw_text(font_small, "<", pad * 2, py, hl_color);

            char label_buf[128];
            snprintf(label_buf, sizeof(label_buf), "%s — %s",
                     p->label, p->description);
            ap_draw_text(font_small, label_buf,
                         pad * 2 + arrow_w + pad, py, text_color);

            /* Right arrow + value */
            int val_w = ap_measure_text(font_small, val_str);
            ap_draw_text(font_small, ">",
                         sw - pad * 2 - arrow_w, py, hl_color);
            ap_draw_text(font_small, val_str,
                         sw - pad * 2 - arrow_w - pad - val_w,
                         py, text_color);
        }

        /* --- Hint line --- */
        {
            pakkit_hint hints[] = {
                { .button = "B", .label = "Quit" },
                { .button = "Y", .label = "Clear" },
                { .button = "A", .label = "Install" },
            };
            pakkit_draw_hints(hints, 3);
        }

        ap_present();
    }

    /* Cleanup */
    if (state.preview_texture) SDL_DestroyTexture(state.preview_texture);
    if (state.gl_initialized) {
        glDeleteProgram(state.gl_program_glow);
        glDeleteProgram(state.gl_program_scan);
        glDeleteTextures(1, &state.gl_source_tex);
        glDeleteTextures(1, &state.gl_fbo_tex);
        glDeleteTextures(1, &state.gl_output_tex);
        glDeleteFramebuffers(1, &state.gl_fbo);
    }

    ap_quit();
    return 0;
}
