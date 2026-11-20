/*
 * Switch Dear ImGui overlay & settings menu (deko3d backend).
 * Implements overlay.h + overlay_switch_* hooks from deko_switch.h.
 */
#include <switch.h>
#include <deko3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "imgui.h"
#include "deko_switch.h"
#include "overlay.h"
extern "C" {
#include "settings.h"
#include "metrics.h"
#include "platform.h"
int gfx_gpu_render_scale_wanted(void);
void gfx_gpu_render_scale_request(int n);
int gfx_gpu_render_scale_max(void);
void window_fps_stats(float *cur, float *mean, uint64_t *n);
}

static DkDevice g_dev;
static DkQueue g_queue;
static int g_ready;
static int g_open, g_paused, g_wants_quit, g_wants_capture;

/* GPU resources for Dear ImGui */
#define IMGUI_CODE_SZ (64 * 1024)
#define IMGUI_DATA_SZ (2 * 1024 * 1024)
static DkMemBlock g_code_mem, g_data_mem, g_tex_mem;
static DkShader g_vsh, g_fsh;
static int g_shaders_ok;
static DkImage g_font_img;
static DkImageDescriptor *g_img_desc;
static DkSamplerDescriptor *g_smp_desc;
static uint32_t g_data_off;

/* Toast state */
struct Toast { char title[64], body[256]; double until; };
static Toast g_toasts[3];
static int g_ntoasts;

static int load_dksh(DkShader *sh, const char *path, uint32_t *code_off) {
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	rewind(f);
	if (sz <= 0 || *code_off + (uint32_t)sz > IMGUI_CODE_SZ - DK_SHADER_CODE_UNUSABLE_SIZE) { fclose(f); return 0; }
	void *tmp = malloc((size_t)sz);
	if (!tmp) { fclose(f); return 0; }
	size_t nr = fread(tmp, 1, (size_t)sz, f);
	fclose(f);
	if (nr != (size_t)sz) { free(tmp); return 0; }
	uint32_t off = *code_off;
	*code_off = (off + (uint32_t)sz + DK_SHADER_CODE_ALIGNMENT - 1) & ~(DK_SHADER_CODE_ALIGNMENT - 1);
	memcpy((uint8_t *)dkMemBlockGetCpuAddr(g_code_mem) + off, tmp, (size_t)sz);
	free(tmp);
	DkShaderMaker sm;
	dkShaderMakerDefaults(&sm, g_code_mem, off);
	dkShaderInitialize(sh, &sm);
	return dkShaderIsValid(sh) ? 1 : 0;
}

extern "C" int overlay_switch_init(DkDevice dev, DkQueue queue) {
	g_dev = dev;
	g_queue = queue;

	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
	io.IniFilename = nullptr;
	ImGui::StyleColorsDark();
	ImGuiStyle &st = ImGui::GetStyle();
	st.ScaleAllSizes(1.4f);

	DkMemBlockMaker mb;
	dkMemBlockMakerDefaults(&mb, g_dev, IMGUI_CODE_SZ);
	mb.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
	g_code_mem = dkMemBlockCreate(&mb);

	uint32_t coff = 0;
	g_shaders_ok = load_dksh(&g_vsh, "romfs:/shaders/imgui_vsh.dksh", &coff) &&
	               load_dksh(&g_fsh, "romfs:/shaders/imgui_fsh.dksh", &coff);

	dkMemBlockMakerDefaults(&mb, g_dev, IMGUI_DATA_SZ);
	mb.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
	g_data_mem = dkMemBlockCreate(&mb);

	/* Upload font atlas */
	unsigned char *pixels = nullptr;
	int fw = 0, fh = 0;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);

	DkImageLayoutMaker lm;
	dkImageLayoutMakerDefaults(&lm, g_dev);
	lm.flags = DkImageFlags_Usage2DEngine;
	lm.format = DkImageFormat_RGBA8_Unorm;
	lm.dimensions[0] = (uint32_t)fw;
	lm.dimensions[1] = (uint32_t)fh;
	DkImageLayout lay;
	dkImageLayoutInitialize(&lay, &lm);
	uint32_t tsz = dkImageLayoutGetSize(&lay);
	uint32_t tal = dkImageLayoutGetAlignment(&lay);
	tsz = (tsz + tal - 1) & ~(tal - 1);

	dkMemBlockMakerDefaults(&mb, g_dev, tsz);
	mb.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
	g_tex_mem = dkMemBlockCreate(&mb);
	dkImageInitialize(&g_font_img, &lay, g_tex_mem, 0);

	/* Descriptors at start of g_data_mem */
	uint8_t *base_cpu = (uint8_t *)dkMemBlockGetCpuAddr(g_data_mem);
	g_img_desc = (DkImageDescriptor *)base_cpu;
	g_smp_desc = (DkSamplerDescriptor *)(base_cpu + DK_IMAGE_DESCRIPTOR_ALIGNMENT);
	uint32_t desc_end = DK_IMAGE_DESCRIPTOR_ALIGNMENT + DK_SAMPLER_DESCRIPTOR_ALIGNMENT;
	desc_end = (desc_end + 255u) & ~255u;

	DkImageView fiv;
	dkImageViewDefaults(&fiv, &g_font_img);
	dkImageDescriptorInitialize(g_img_desc, &fiv, false, false);

	DkSampler smp;
	dkSamplerDefaults(&smp);
	smp.minFilter = DkFilter_Linear;
	smp.magFilter = DkFilter_Linear;
	dkSamplerDescriptorInitialize(g_smp_desc, &smp);

	/* Stage font pixels and upload once */
	size_t pbytes = (size_t)fw * (size_t)fh * 4;
	uint32_t stg_off = desc_end;
	if (stg_off + pbytes <= IMGUI_DATA_SZ) {
		memcpy(base_cpu + stg_off, pixels, pbytes);
		DkMemBlock cmdm;
		dkMemBlockMakerDefaults(&mb, g_dev, 16 * 1024);
		mb.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
		cmdm = dkMemBlockCreate(&mb);
		DkCmdBufMaker cm;
		dkCmdBufMakerDefaults(&cm, g_dev);
		DkCmdBuf cmd = dkCmdBufCreate(&cm);
		dkCmdBufAddMemory(cmd, cmdm, 0, 16 * 1024);
		DkCopyBuf cb = { dkMemBlockGetGpuAddr(g_data_mem) + stg_off, 0, 0 };
		DkImageRect rc = { 0, 0, 0, (uint32_t)fw, (uint32_t)fh, 1 };
		dkCmdBufCopyBufferToImage(cmd, &cb, &fiv, &rc, 0);
		dkQueueSubmitCommands(g_queue, dkCmdBufFinishList(cmd));
		dkQueueWaitIdle(g_queue);
		dkCmdBufDestroy(cmd);
		dkMemBlockDestroy(cmdm);
	}
	g_data_off = desc_end;
	io.Fonts->SetTexID((ImTextureID)(uintptr_t)1);
	g_ready = 1;
	return 1;
}

extern "C" void overlay_switch_shutdown(void) {
	if (!g_ready) return;
	g_ready = 0;
	ImGui::DestroyContext();
	if (g_tex_mem) dkMemBlockDestroy(g_tex_mem);
	if (g_data_mem) dkMemBlockDestroy(g_data_mem);
	if (g_code_mem) dkMemBlockDestroy(g_code_mem);
}

extern "C" void overlay_switch_input(uint64_t buttons, int touch_down, float tx, float ty) {
	if (!g_ready) return;
	ImGuiIO &io = ImGui::GetIO();
	io.AddMousePosEvent(tx, ty);
	io.AddMouseButtonEvent(0, touch_down != 0);
	io.AddKeyEvent(ImGuiKey_GamepadDpadUp, (buttons & HidNpadButton_Up) != 0);
	io.AddKeyEvent(ImGuiKey_GamepadDpadDown, (buttons & HidNpadButton_Down) != 0);
	io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, (buttons & HidNpadButton_Left) != 0);
	io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (buttons & HidNpadButton_Right) != 0);
	io.AddKeyEvent(ImGuiKey_GamepadFaceDown, (buttons & HidNpadButton_A) != 0);
	io.AddKeyEvent(ImGuiKey_GamepadFaceRight, (buttons & HidNpadButton_B) != 0);
	io.AddKeyEvent(ImGuiKey_GamepadL1, (buttons & HidNpadButton_L) != 0);
	io.AddKeyEvent(ImGuiKey_GamepadR1, (buttons & HidNpadButton_R) != 0);
}

extern "C" void overlay_switch_render(DkCmdBuf cmd, const DkImageView *target, uint32_t w, uint32_t h, double dt_s) {
	if (!g_ready || !g_shaders_ok) return;
	double now = (double)plat_now_us() / 1e6;
	int have_toasts = 0;
	for (int i = 0; i < g_ntoasts; i++) if (g_toasts[i].until > now) have_toasts = 1;
	if (!g_open && !have_toasts) return;

	ImGuiIO &io = ImGui::GetIO();
	io.DisplaySize = ImVec2((float)w, (float)h);
	io.DeltaTime = dt_s > 0.0 ? (float)dt_s : (1.0f / 60.0f);

	ImGui::NewFrame();

	if (have_toasts) {
		ImGui::SetNextWindowPos(ImVec2((float)w - 20.0f, 20.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
		ImGui::Begin("##toasts", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
		for (int i = 0; i < g_ntoasts; i++) {
			if (g_toasts[i].until <= now) continue;
			if (g_toasts[i].title[0]) ImGui::TextUnformatted(g_toasts[i].title);
			ImGui::TextWrapped("%s", g_toasts[i].body);
		}
		ImGui::End();
	}

	if (g_open) {
		ImGui::SetNextWindowPos(ImVec2((float)w * 0.5f, (float)h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(520.0f, 380.0f), ImGuiCond_Always);
		if (ImGui::Begin("Project Nativ (Switch / deko3d)", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
			float fcur = 0, fmean = 0; uint64_t fn = 0;
			window_fps_stats(&fcur, &fmean, &fn);
			ImGui::Text("FPS: %.1f (avg %.1f)  |  %ux%u", fcur, fmean, w, h);
			ImGui::Separator();

			int rs = gfx_gpu_render_scale_wanted();
			int rmax = gfx_gpu_render_scale_max();
			if (rmax < 1) rmax = 1;
			if (ImGui::SliderInt("Render scale", &rs, 1, rmax)) gfx_gpu_render_scale_request(rs);

			float vol = (float)settings_float("volume", 1.0);
			if (ImGui::SliderFloat("Volume", &vol, 0.0f, 1.0f)) {
				settings_set_float("volume", vol);
				settings_save();
			}

			ImGui::Spacing();
			if (ImGui::Button("Resume", ImVec2(160, 0))) overlay_close();
			ImGui::SameLine();
			if (ImGui::Button("Quit", ImVec2(160, 0))) g_wants_quit = 1;
		}
		ImGui::End();
	}

	ImGui::Render();
	ImDrawData *dd = ImGui::GetDrawData();
	if (!dd || dd->TotalVtxCount == 0) return;

	/* Ring-allocate projection UBO + vertex/index data in g_data_mem */
	uint32_t desc_base = DK_IMAGE_DESCRIPTOR_ALIGNMENT + DK_SAMPLER_DESCRIPTOR_ALIGNMENT;
	desc_base = (desc_base + 255u) & ~255u;
	uint8_t *cpu = (uint8_t *)dkMemBlockGetCpuAddr(g_data_mem);
	DkGpuAddr gpu = dkMemBlockGetGpuAddr(g_data_mem);

	size_t vbytes = (size_t)dd->TotalVtxCount * sizeof(ImDrawVert);
	size_t ibytes = (size_t)dd->TotalIdxCount * sizeof(ImDrawIdx);
	size_t need = 256 + ((vbytes + 15) & ~15u) + ((ibytes + 15) & ~15u);
	if (g_data_off + need > IMGUI_DATA_SZ) g_data_off = desc_base;

	uint32_t ubo_off = g_data_off;
	float L = dd->DisplayPos.x, R = dd->DisplayPos.x + dd->DisplaySize.x;
	float T = dd->DisplayPos.y, B = dd->DisplayPos.y + dd->DisplaySize.y;
	float ortho[4][4] = {
		{ 2.0f / (R - L), 0, 0, 0 },
		{ 0, 2.0f / (T - B), 0, 0 },
		{ 0, 0, 1.0f, 0 },
		{ (R + L) / (L - R), (T + B) / (B - T), 0, 1.0f },
	};
	memcpy(cpu + ubo_off, ortho, sizeof ortho);

	uint32_t voff = ubo_off + 256;
	uint32_t ioff = (uint32_t)((voff + vbytes + 15) & ~15u);
	ImDrawVert *vd = (ImDrawVert *)(cpu + voff);
	ImDrawIdx *id = (ImDrawIdx *)(cpu + ioff);
	for (int n = 0; n < dd->CmdListsCount; n++) {
		const ImDrawList *cl = dd->CmdLists[n];
		memcpy(vd, cl->VtxBuffer.Data, (size_t)cl->VtxBuffer.Size * sizeof(ImDrawVert));
		memcpy(id, cl->IdxBuffer.Data, (size_t)cl->IdxBuffer.Size * sizeof(ImDrawIdx));
		vd += cl->VtxBuffer.Size;
		id += cl->IdxBuffer.Size;
	}
	g_data_off = (ioff + (uint32_t)ibytes + 255u) & ~255u;

	DkImageView const *targets[] = { target };
	dkCmdBufBindRenderTargets(cmd, targets, 1, NULL);
	DkViewport vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
	dkCmdBufSetViewports(cmd, 0, &vp, 1);

	DkShader const *shs[] = { &g_vsh, &g_fsh };
	dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shs, 2);

	DkRasterizerState rs; dkRasterizerStateDefaults(&rs); rs.cullMode = DkFace_None;
	DkColorState cs; dkColorStateDefaults(&cs);
	DkColorWriteState cws; dkColorWriteStateDefaults(&cws);
	DkBlendState bs; dkBlendStateDefaults(&bs);
	bs.srcColorBlendFactor = DkBlendFactor_SrcAlpha;
	bs.dstColorBlendFactor = DkBlendFactor_InvSrcAlpha;
	bs.srcAlphaBlendFactor = DkBlendFactor_One;
	bs.dstAlphaBlendFactor = DkBlendFactor_InvSrcAlpha;
	cs.blendEnableMask = 1;
	DkDepthStencilState dss; dkDepthStencilStateDefaults(&dss); dss.depthTestEnable = false; dss.depthWriteEnable = false;
	dkCmdBufBindRasterizerState(cmd, &rs);
	dkCmdBufBindColorState(cmd, &cs);
	dkCmdBufBindColorWriteState(cmd, &cws);
	dkCmdBufBindBlendStates(cmd, 0, &bs, 1);
	dkCmdBufBindDepthStencilState(cmd, &dss);

	DkVtxAttribState attribs[3] = {
		{ 0, 0, offsetof(ImDrawVert, pos), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0 },
		{ 0, 0, offsetof(ImDrawVert, uv),  DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0 },
		{ 0, 0, offsetof(ImDrawVert, col), DkVtxAttribSize_4x8,  DkVtxAttribType_Unorm, 0 },
	};
	DkVtxBufferState vbs = { sizeof(ImDrawVert), 0 };
	dkCmdBufBindVtxAttribState(cmd, attribs, 3);
	dkCmdBufBindVtxBufferState(cmd, &vbs, 1);
	DkBufExtents vext = { gpu + voff, (uint32_t)vbytes };
	dkCmdBufBindVtxBuffers(cmd, 0, &vext, 1);
	dkCmdBufBindIdxBuffer(cmd, sizeof(ImDrawIdx) == 2 ? DkIdxFormat_Uint16 : DkIdxFormat_Uint32, gpu + ioff);
	dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 0, gpu + ubo_off, 256);
	dkCmdBufBindImageDescriptorSet(cmd, gpu, 1);
	dkCmdBufBindSamplerDescriptorSet(cmd, gpu + DK_IMAGE_DESCRIPTOR_ALIGNMENT, 1);
	DkResHandle th = dkMakeTextureHandle(0, 0);
	dkCmdBufBindTextures(cmd, DkStage_Fragment, 0, &th, 1);

	int vtx_off = 0, idx_off = 0;
	for (int n = 0; n < dd->CmdListsCount; n++) {
		const ImDrawList *cl = dd->CmdLists[n];
		for (int i = 0; i < cl->CmdBuffer.Size; i++) {
			const ImDrawCmd *pc = &cl->CmdBuffer[i];
			int sx0 = pc->ClipRect.x < 0 ? 0 : (int)pc->ClipRect.x;
			int sy0 = pc->ClipRect.y < 0 ? 0 : (int)pc->ClipRect.y;
			int sx1 = pc->ClipRect.z > (float)w ? (int)w : (int)pc->ClipRect.z;
			int sy1 = pc->ClipRect.w > (float)h ? (int)h : (int)pc->ClipRect.w;
			if (sx1 <= sx0 || sy1 <= sy0) continue;
			DkScissor sc = { (uint32_t)sx0, (uint32_t)sy0, (uint32_t)(sx1 - sx0), (uint32_t)(sy1 - sy0) };
			dkCmdBufSetScissors(cmd, 0, &sc, 1);
			dkCmdBufDrawIndexed(cmd, DkPrimitive_Triangles, pc->ElemCount, 1, (uint32_t)(idx_off + pc->IdxOffset), (int32_t)(vtx_off + pc->VtxOffset), 0);
		}
		vtx_off += cl->VtxBuffer.Size;
		idx_off += cl->IdxBuffer.Size;
	}
}

extern "C" {
void overlay_toast_titled(const char *title, const char *body, double seconds) {
	double until = (double)plat_now_us() / 1e6 + (seconds > 0 ? seconds : 3.0);
	int slot = g_ntoasts < 3 ? g_ntoasts++ : 0;
	snprintf(g_toasts[slot].title, sizeof g_toasts[slot].title, "%s", title ? title : "");
	snprintf(g_toasts[slot].body, sizeof g_toasts[slot].body, "%s", body ? body : "");
	g_toasts[slot].until = until;
}
void overlay_toast(const char *msg, double seconds) { overlay_toast_titled("", msg, seconds); }
int overlay_dialog_active(void) { return 0; }
void overlay_shutdown(void) { overlay_switch_shutdown(); }
int overlay_is_open(void) { return g_open; }
int overlay_layout_ask(void) { return 1; }
void overlay_open(void) { g_open = 1; }
void overlay_close(void) { g_open = 0; g_paused = 0; }
void overlay_toggle(void) { if (g_open) overlay_close(); else overlay_open(); }
void overlay_set_paused(int on) { g_paused = on != 0; }
int overlay_game_paused(void) { return g_open || g_paused; }
int overlay_mouse_captured(void) { return g_open; }
void overlay_bar_dismiss(void) {}
int overlay_wants_quit(void) { return g_wants_quit; }
int overlay_quit_confirm(void) { g_wants_quit = 1; return 0; }
int overlay_wants_capture(void) { int r = g_wants_capture; g_wants_capture = 0; return r; }
void overlay_touch_cursor(int on) { (void)on; }
int overlay_draws_hud(void) { return 0; }
}
