// TOTK: lsfg_c.h on top of lsfg-vk's framegen (LSFG 3.1 and 3.1p) and its Lossless.dll resource parser.
#include "lsfg_c.h"
#include "extract/dll.hpp"

#include <lsfg_3_1.hpp>
#include <lsfg_3_1p.hpp>

#include <cstdio>
#include <exception>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" PFN_vkGetInstanceProcAddr lsfg_custom_gipa;
PFN_vkGetInstanceProcAddr lsfg_custom_gipa = nullptr;

namespace {
// lsfg-vk's src/extract/extract.cpp: shader name -> resource id of the FP16 variant (FP32 is 49 further on)
const uint32_t NO = 49, PO = NO + 23, FP = 49;
const std::unordered_map<std::string, uint32_t> nameIdx = {
    {"mipmaps", 255 + NO}, {"alpha[0]", 267 + NO}, {"alpha[1]", 268 + NO}, {"alpha[2]", 269 + NO}, {"alpha[3]", 270 + NO},
    {"beta[0]", 275 + NO}, {"beta[1]", 276 + NO}, {"beta[2]", 277 + NO}, {"beta[3]", 278 + NO}, {"beta[4]", 279 + NO},
    {"gamma[0]", 257 + NO}, {"gamma[1]", 259 + NO}, {"gamma[2]", 260 + NO}, {"gamma[3]", 261 + NO}, {"gamma[4]", 262 + NO},
    {"delta[0]", 257 + NO}, {"delta[1]", 263 + NO}, {"delta[2]", 264 + NO}, {"delta[3]", 265 + NO}, {"delta[4]", 266 + NO},
    {"delta[5]", 258 + NO}, {"delta[6]", 271 + NO}, {"delta[7]", 272 + NO}, {"delta[8]", 273 + NO}, {"delta[9]", 274 + NO},
    {"generate", 256 + NO},
    {"p_alpha[0]", 267 + PO}, {"p_alpha[1]", 268 + PO}, {"p_alpha[2]", 269 + PO}, {"p_alpha[3]", 270 + PO},
    {"p_beta[0]", 275 + PO}, {"p_beta[1]", 276 + PO}, {"p_beta[2]", 277 + PO}, {"p_beta[3]", 278 + PO}, {"p_beta[4]", 279 + PO},
    {"p_gamma[0]", 257 + PO}, {"p_gamma[1]", 259 + PO}, {"p_gamma[2]", 260 + PO}, {"p_gamma[3]", 261 + PO}, {"p_gamma[4]", 262 + PO},
    {"p_delta[0]", 257 + PO}, {"p_delta[1]", 263 + PO}, {"p_delta[2]", 264 + PO}, {"p_delta[3]", 265 + PO}, {"p_delta[4]", 266 + PO},
    {"p_delta[5]", 258 + PO}, {"p_delta[6]", 271 + PO}, {"p_delta[7]", 272 + PO}, {"p_delta[8]", 273 + PO}, {"p_delta[9]", 274 + PO},
};
std::unordered_map<uint32_t, std::vector<uint8_t>> g_res;
int g_perf;

int fail(char *err, size_t errn, const char *what) { if (err && errn) std::snprintf(err, errn, "%s", what); return -1; }

std::vector<uint8_t> shader(const std::string &name, bool fp16) {
    auto it = nameIdx.find(name);
    if (it == nameIdx.end()) throw std::runtime_error("unknown shader " + name);
    auto r = g_res.find(it->second + (fp16 ? 0 : FP));
    if (r == g_res.end()) throw std::runtime_error("Lossless.dll lacks the shader " + name);
    return r->second;
}
}

extern "C" int lsfg_init(PFN_vkGetInstanceProcAddr gipa, uint64_t device_uuid, const char *dll, int perf, float flow_scale,
                         int generated, int no_fp16, char *err, size_t errn) {
    try {
        g_res = DLL::parse_dll(dll);
        for (const auto &[name, idx] : nameIdx)
            if (!g_res.count(idx) || !g_res.count(idx + FP)) return fail(err, errn, ("Lossless.dll lacks the shader " + name + " (not the version lsfg-vk 1.0 reads)").c_str());
        lsfg_custom_gipa = gipa; g_perf = perf;
        if (perf) LSFG_3_1P::initialize(device_uuid, false, 1.0f / flow_scale, (uint64_t)generated, no_fp16 != 0, shader);
        else LSFG_3_1::initialize(device_uuid, false, 1.0f / flow_scale, (uint64_t)generated, no_fp16 != 0, shader);
        return 0;
    } catch (const std::exception &e) { return fail(err, errn, e.what()); }
}
extern "C" int lsfg_create(int in0, int in1, const int *out, int nout, uint32_t w, uint32_t h, VkFormat format, char *err, size_t errn) {
    try {
        std::vector<int> o(out, out + nout);
        return g_perf ? LSFG_3_1P::createContext(in0, in1, o, {w, h}, format) : LSFG_3_1::createContext(in0, in1, o, {w, h}, format);
    } catch (const std::exception &e) { return fail(err, errn, e.what()); }
}
extern "C" int lsfg_present(int ctx, int in_sem, int *out_sems, int nout, char *err, size_t errn) {
    try {
        std::vector<int> o(nout, -1);
        if (g_perf) LSFG_3_1P::presentContext(ctx, in_sem, o); else LSFG_3_1::presentContext(ctx, in_sem, o);
        for (int i = 0; i < nout; i++) out_sems[i] = o[i];
        return 0;
    } catch (const std::exception &e) { return fail(err, errn, e.what()); }
}
extern "C" void lsfg_delete(int ctx) {
    try { if (g_perf) LSFG_3_1P::deleteContext(ctx); else LSFG_3_1::deleteContext(ctx); } catch (...) {}
}
extern "C" void lsfg_finalize(void) {
    try { if (g_perf) LSFG_3_1P::finalize(); else LSFG_3_1::finalize(); } catch (...) {}
}
