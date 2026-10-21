/* Native SDK transport correlation fixtures; never identity authority. */
static atomic_int sdk_metadata_case, sdk_host_metadata;
static int sdk_metadata_refuses(int mode)
{
   return (mode >= 4 && mode <= 9) || mode == 11;
}
static const char *sdk_metadata_value(void)
{
   static _Thread_local char value[256];
   snprintf(value, sizeof value,
            "{\"device_id\":\"sdk-device\",\"account_uuid\":\"\",\"session_id\":\"sdk-%d\"}",
            atomic_load(&snapshot_generation));
   return value;
}
static cJSON *sdk_metadata_fixture(int mode)
{
   if (mode == 1)
      return NULL;
   if (mode == 5)
      return cJSON_CreateArray();
   if (mode == 6)
      return cJSON_Parse("{\"user_id\":1}");
   if (mode == 7)
      return cJSON_Parse("{\"actor\":\"foreign\"}");
   cJSON *metadata = cJSON_CreateObject();
   assert(metadata);
   if (mode == 2)
      cJSON_AddNullToObject(metadata, "user_id");
   else if (mode == 3)
   {
      char unicode[1537];
      for (int i = 0; i < 512; i++)
         memcpy(unicode + i * 3, "\xe2\x82\xac", 3);
      unicode[1536] = 0;
      cJSON_AddStringToObject(metadata, "user_id", unicode);
   }
   else if (mode == 4)
   {
      char too_long[514];
      memset(too_long, 'x', 513);
      too_long[513] = 0;
      cJSON_AddStringToObject(metadata, "user_id", too_long);
   }
   else if (mode == 11)
      cJSON_AddStringToObject(metadata, "user_id", "\xc0\x80");
   else
      cJSON_AddStringToObject(metadata, "user_id", sdk_metadata_value());
   if (mode == 8)
      cJSON_AddStringToObject(metadata, "user_id", "duplicate");
   if (mode == 12)
      cJSON_AddStringToObject(metadata, "codex_extension", "unchanged prior handling");
   return metadata;
}
