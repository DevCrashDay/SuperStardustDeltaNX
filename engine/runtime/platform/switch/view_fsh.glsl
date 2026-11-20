#version 460
layout(std140, binding = 0) uniform ViewU {
	uint off_bytes, tex_stride, tgt_stride, tex_bpp, tgt_bpp, mode, tgt_w, tgt_h;
	uvec4 swz;
	uint scale, scale_y, ms_x, ms_y;
} u;
layout(binding = 0) uniform sampler2D src;
layout(location = 0) out vec4 o;

vec4 swizzle(vec4 c, uvec4 z) {
	vec4 r;
	for (int k = 0; k < 4; k++) r[k] = z[k] < 4u ? c[int(z[k])] : z[k] == 4u ? 0.0 : 1.0;
	return r;
}
uint tsf(float f, uint mbits) {
	if (!(f > 0.0)) return 0u;
	if (f >= 65504.0) return (30u << mbits) | ((1u << mbits) - 1u);
	int e; float m = frexp(f, e); int be = e - 1 + 15;
	if (be <= 0) return uint(f * ldexp(1.0, 14 + int(mbits)) + 0.5) & ((1u << mbits) - 1u);
	uint mant = uint((m * 2.0 - 1.0) * float(1u << mbits) + 0.5);
	if ((mant >> mbits) != 0u) { mant = 0u; be++; }
	return (uint(be) << mbits) | mant;
}
float sfl(uint v, uint mbits) {
	uint e = v >> mbits, m = v & ((1u << mbits) - 1u);
	if (e == 0u) return ldexp(float(m), -14 - int(mbits));
	if (e == 31u) return m != 0u ? 0.0 : 65504.0;
	return ldexp(1.0 + float(m) / float(1u << mbits), int(e) - 15);
}
float q(float f, uint mbits) { return sfl(tsf(f, mbits), mbits); }
uvec2 srcpix(uvec2 P, out uint sub) {
	uvec2 sc = uvec2(u.scale, u.scale_y), p = P / sc, f = P % sc;
	uint b = u.off_bytes + (p.y * u.tex_stride + p.x) * u.tex_bpp;
	uint pix = b / u.tgt_bpp; sub = (b % u.tgt_bpp) / 4u;
	return min(uvec2(pix % u.tgt_stride, pix / u.tgt_stride) / max(uvec2(u.ms_x, u.ms_y), uvec2(1u)), uvec2(u.tgt_w - 1u, u.tgt_h - 1u)) * sc + f;
}
float s8(uint bb) { int v = int(bb); if (v >= 128) v -= 256; return v <= -127 ? -1.0 : float(v) / 127.0; }
vec3 enc(vec3 x) { return mix(1.055 * pow(x, vec3(1.0 / 2.4)) - 0.055, 12.92 * x, lessThanEqual(x, vec3(0.0031308))); }
vec3 dec(vec3 x) { return mix(pow((x + 0.055) / 1.055, vec3(2.4)), x / 12.92, lessThanEqual(x, vec3(0.04045))); }

void main() {
	if (u.mode == 7u || u.mode == 8u) {
		uvec2 sc = uvec2(u.scale, u.scale_y), P = uvec2(gl_FragCoord.xy), p = P / sc, f = P % sc;
		uint o0 = u.off_bytes / 4u;
		uvec2 org = uvec2(o0 % u.tgt_stride, o0 / u.tgt_stride) + 2u * p, lim = uvec2(u.tgt_w - 1u, u.tgt_h - 1u);
		uvec4 s = uvec4(0u);
		for (uint d = 0u; d < 4u; d++) {
			vec4 c = texelFetch(src, ivec2(min(org + uvec2(d & 1u, d >> 1u), lim) * sc + f), 0);
			if (u.mode == 8u) c.rgb = enc(c.rgb);
			s += uvec4(clamp(c, 0.0, 1.0) * 255.0 + 0.5);
		}
		vec4 r = vec4((s + 2u) / 4u) / 255.0;
		if (u.mode == 8u) r.rgb = dec(r.rgb);
		o = r;
		return;
	}
	uint sub;
	uvec2 sp = srcpix(uvec2(gl_FragCoord.xy), sub);
	vec4 c = texelFetch(src, ivec2(sp), 0);
	if (u.mode == 5u || u.mode == 6u) { o = swizzle(vec4(c.r, 0.0, 0.0, 1.0), u.swz); return; }
	if (u.mode == 1u) { o = vec4(q(c.r, 6u), q(c.g, 6u), q(c.b, 5u), 1.0); return; }
	if (u.mode == 2u) {
		float al = c.a <= 0.0 ? 0.0 : c.a >= 1.0 ? 3.0 : floor(c.a * 3.0 + 0.5);
		o = vec4(q(c.r, 5u), q(c.g, 5u), q(c.b, 5u), al / 3.0);
		return;
	}
	if (u.mode == 3u || u.mode == 4u) {
		uint w = floatBitsToUint(sub == 1u ? c.g : sub == 2u ? c.b : sub == 3u ? c.a : c.r);
		vec4 r;
		for (int k = 0; k < 4; k++) { uint bb = (w >> (8u * uint(k))) & 255u; r[k] = u.mode == 3u ? float(bb) / 255.0 : s8(bb); }
		o = swizzle(r, u.swz);
		return;
	}
	o = swizzle(c, u.swz);
}
