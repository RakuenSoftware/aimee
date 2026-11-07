/* config_test_setters.c: test-only config setters; see config_test_setters.h. */
#include "config_test_setters.h"
#include "config_client.h"

int config_set_calibration_buckets(int value)
{
   return config_client_set_number("calibration_buckets", (double)value);
}

int config_set_calibration_conformal_window(int value)
{
   return config_client_set_number("calibration_conformal_window", (double)value);
}

int config_set_calibration_enabled(int value)
{
   return config_client_set_number("calibration_enabled", (double)value);
}

int config_set_calibration_model_version(const char *value)
{
   return config_client_set_string("calibration_model_version", value);
}

int config_set_calibration_prompt_version(const char *value)
{
   return config_client_set_string("calibration_prompt_version", value);
}

int config_set_calibration_tau_memory_auto(double value)
{
   return config_client_set_number("calibration_tau_memory_auto", (double)value);
}

int config_set_calibration_tau_memory_flag(double value)
{
   return config_client_set_number("calibration_tau_memory_flag", (double)value);
}

int config_set_calibration_tau_working_profile_auto(double value)
{
   return config_client_set_number("calibration_tau_working_profile_auto", (double)value);
}

int config_set_calibration_tau_working_profile_flag(double value)
{
   return config_client_set_number("calibration_tau_working_profile_flag", (double)value);
}

int config_set_delegate_sandbox_image(const char *value)
{
   return config_client_set_string("delegate_sandbox_image", value);
}

int config_set_ensemble_aggregator(const char *value)
{
   return config_client_set_string("ensemble_aggregator", value);
}

int config_set_kb_curator_extract_docs_enabled(int value)
{
   return config_client_set_number("kb_curator_extract_docs_enabled", (double)value);
}

int config_set_kb_mode(const char *value)
{
   return config_client_set_string("kb_mode", value);
}

int config_set_kb_pdf_assets_enabled(int value)
{
   return config_client_set_number("kb_pdf_assets_enabled", (double)value);
}

int config_set_kb_pdf_ocr_enabled(int value)
{
   return config_client_set_number("kb_pdf_ocr_enabled", (double)value);
}

int config_set_memory_recall_enabled(int value)
{
   return config_client_set_number("memory_recall_enabled", (double)value);
}

int config_set_provider(const char *value)
{
   return config_client_set_string("provider", value);
}

int config_set_reasoning_datalog_command(const char *value)
{
   return config_client_set_string("reasoning_datalog_command", value);
}

int config_set_require_session_worktree(int value)
{
   return config_client_set_number("require_session_worktree", (double)value);
}

int config_set_server_api_mtls(int value)
{
   return config_client_set_number("server_api_mtls", (double)value);
}

int config_set_workspace_count(int value)
{
   return config_client_set_number("workspace_count", (double)value);
}
