#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "db1.h"
#include "hardware_probe.h"

static void test_nvidia_csv_parse(void)
{
   hardware_probe_result_t hw;
   assert(hardware_probe_parse_nvidia_csv("RTX 4090, 24564\nRTX 3060, 12288\n", &hw) == 0);
   assert(hw.detected == 1);
   assert(hw.vram_mb == 24564);
   assert(strcmp(hw.vendor, "nvidia") == 0);
   assert(strcmp(hw.memory_kind, "discrete_vram") == 0);
   assert(strcmp(hw.name, "RTX 4090") == 0);
}

static void test_amd_vram_parse(void)
{
   int mb = 0;
   assert(hardware_probe_parse_amd_vram_bytes("17179869184\n", &mb) == 0);
   assert(mb == 16384);
   assert(hardware_probe_parse_amd_vram_bytes("not-a-number", &mb) != 0);
}

static void test_vram_context_tiers(void)
{
   assert(hardware_probe_context_window_from_vram(0, 0) == 0);
   assert(hardware_probe_context_window_from_vram(6144, 0) == 4096);
   assert(hardware_probe_context_window_from_vram(8192, 0) == 8192);
   assert(hardware_probe_context_window_from_vram(12288, 0) == 16384);
   assert(hardware_probe_context_window_from_vram(24576, 0) == 65536);
   assert(hardware_probe_context_window_from_vram(24576, 32768) == 32768);
}

static void test_model_estimate(void)
{
   assert(hardware_probe_quant_bpw("Qwen2.5-7B-Q4_K_M.gguf") > 4.0);
   assert(hardware_probe_params_billion("gemma-4-26B-A4B-it-UD-Q5_K_XL.gguf") == 26.0);
   int estimate = hardware_probe_estimate_model_vram_mb("Qwen_Qwen3.6-35B-A3B-Q5_K_M.gguf", 32768);
   assert(estimate > 20000);

   hardware_probe_result_t hw;
   hardware_probe_result_init(&hw);
   hw.detected = 1;
   hw.vram_mb = 8192;
   snprintf(hw.vendor, sizeof(hw.vendor), "nvidia");
   int estimate_out = 0;
   assert(hardware_probe_should_warn_fit(&hw, "Qwen_Qwen3.6-35B-A3B-Q5_K_M.gguf", 32768,
                                         &estimate_out) == 1);
   assert(estimate_out == estimate);
}

static void test_env_capability_get(void)
{
   assert(db1_init(":memory:") == 0);
   assert(db1_env_capability_get("gpu_vram_mb", NULL, 0, NULL, 0) == 0);
   assert(db1_env_capability_set("gpu_vram_mb", "24564") == 0);
   char value[64];
   assert(db1_env_capability_get("gpu_vram_mb", value, sizeof(value), NULL, 0) == 1);
   assert(strcmp(value, "24564") == 0);
   db1_shutdown();
}

static void test_cached_no_gpu_is_terminal(void)
{
   assert(db1_init(":memory:") == 0);
   hardware_probe_result_t hw;
   hardware_probe_result_init(&hw);
   hw.detected = 0;
   assert(hardware_probe_cache_result(&hw) == 0);

   hardware_probe_result_t cached;
   assert(hardware_probe_cached_or_detect(&cached) == 0);
   assert(cached.detected == 0);
   assert(cached.vram_mb == 0);
   assert(strstr(cached.error, "no NVIDIA or AMD") != NULL);
   db1_shutdown();
}

static void test_gpu_csv_list_parse(void)
{
   hardware_gpu_list_t gl;

   /* Two NVIDIA GPUs (the uniform probe appends the vendor field). */
   int n = hardware_probe_parse_gpu_csv_list("0, NVIDIA GeForce RTX 4090, 24564, nvidia\n"
                                             "1, NVIDIA GeForce RTX 3060, 12288, nvidia\n",
                                             NULL, &gl);
   assert(n == 2 && gl.count == 2);
   assert(gl.gpus[0].index == 0 && gl.gpus[0].vram_mb == 24564);
   assert(strcmp(gl.gpus[0].name, "NVIDIA GeForce RTX 4090") == 0);
   assert(strcmp(gl.gpus[0].vendor, "nvidia") == 0);
   assert(gl.gpus[1].index == 1 && gl.gpus[1].vram_mb == 12288);

   /* AMD, and a name that itself contains a comma must still split correctly. */
   n = hardware_probe_parse_gpu_csv_list("0, Radeon RX 7900 XTX, Rev A, 24560, amd\n", NULL, &gl);
   assert(n == 1);
   assert(strcmp(gl.gpus[0].name, "Radeon RX 7900 XTX, Rev A") == 0);
   assert(gl.gpus[0].vram_mb == 24560);
   assert(strcmp(gl.gpus[0].vendor, "amd") == 0);

   /* Blank + malformed lines are skipped; empty input => no GPUs, no crash. */
   n = hardware_probe_parse_gpu_csv_list("\n  \ngarbage\n0, x, notanumber, nvidia\n", NULL, &gl);
   assert(n == 0 && gl.count == 0);
   assert(hardware_probe_parse_gpu_csv_list("", NULL, &gl) == 0);

   /* vendor_hint fills in when the vendor field is present but empty. */
   n = hardware_probe_parse_gpu_csv_list("0, Some GPU, 8192, \n", "amd", &gl);
   assert(n == 1 && gl.gpus[0].vram_mb == 8192 && strcmp(gl.gpus[0].vendor, "amd") == 0);
}

int main(void)
{
   test_nvidia_csv_parse();
   test_gpu_csv_list_parse();
   test_amd_vram_parse();
   test_vram_context_tiers();
   test_model_estimate();
   test_env_capability_get();
   test_cached_no_gpu_is_terminal();
   printf("hardware_probe: all tests passed\n");
   return 0;
}
