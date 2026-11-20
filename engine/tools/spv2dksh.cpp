/*
 * Host tool: converts a SPIR-V module emitted by usse_spirv.c into deko3d-compatible GLSL 460
 * via SPIRV-Cross (remapping Vulkan descriptor sets into per-stage deko3d bindings).
 *
 * Usage: spv2dksh <in.spv> <out.glsl> <vert|frag> <nsmp> <nsbuf>
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "spirv_glsl.hpp"

int main(int argc, char **argv) {
	if (argc < 6) {
		fprintf(stderr, "usage: %s <in.spv> <out.glsl> <vert|frag> <nsmp> <nsbuf>\n", argv[0]);
		return 1;
	}
	const char *in_path = argv[1];
	const char *out_path = argv[2];
	bool is_frag = strcmp(argv[3], "frag") == 0;
	int nsmp = atoi(argv[4]);
	(void)argv[5];

	FILE *f = fopen(in_path, "rb");
	if (!f) { perror(in_path); return 1; }
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	rewind(f);
	if (sz <= 0 || (sz & 3)) { fprintf(stderr, "%s: invalid size %ld\n", in_path, sz); fclose(f); return 1; }
	std::vector<uint32_t> spv((size_t)sz / 4);
	if (fread(spv.data(), 4, spv.size(), f) != spv.size()) { fclose(f); return 1; }
	fclose(f);

	try {
		spirv_cross::CompilerGLSL glsl(std::move(spv));
		spirv_cross::CompilerGLSL::Options opts;
		opts.version = 460;
		opts.es = false;
		opts.vulkan_semantics = false;
		opts.enable_420pack_extension = true;
		glsl.set_common_options(opts);

		auto res = glsl.get_shader_resources();
		/* Remap UBOs: vertex set 1 binding b -> b; fragment set 3 binding b -> b */
		for (auto &u : res.uniform_buffers) {
			uint32_t b = glsl.get_decoration(u.id, spv_private::DecorationBinding);
			glsl.unset_decoration(u.id, spv_private::DecorationDescriptorSet);
			glsl.set_decoration(u.id, spv_private::DecorationBinding, b);
		}
		/* Remap SSBOs: vertex set 0 binding nsmp+k -> k; fragment set 2 binding b -> 0.. */
		uint32_t f_ssbo = 0;
		for (auto &s : res.storage_buffers) {
			uint32_t b = glsl.get_decoration(s.id, spv_private::DecorationBinding);
			glsl.unset_decoration(s.id, spv_private::DecorationDescriptorSet);
			uint32_t nb = !is_frag ? (b >= (uint32_t)nsmp ? b - (uint32_t)nsmp : b) : f_ssbo++;
			glsl.set_decoration(s.id, spv_private::DecorationBinding, nb);
		}
		/* Remap sampled images: set 0 (vert) or set 2 (frag), binding i -> i */
		for (auto &si : res.sampled_images) {
			uint32_t b = glsl.get_decoration(si.id, spv_private::DecorationBinding);
			glsl.unset_decoration(si.id, spv_private::DecorationDescriptorSet);
			glsl.set_decoration(si.id, spv_private::DecorationBinding, b);
		}

		std::string src = glsl.compile();
		FILE *out = fopen(out_path, "wb");
		if (!out) { perror(out_path); return 1; }
		fwrite(src.data(), 1, src.size(), out);
		fclose(out);
		return 0;
	} catch (const std::exception &e) {
		fprintf(stderr, "spv2dksh: %s: %s\n", in_path, e.what());
		return 2;
	}
}
