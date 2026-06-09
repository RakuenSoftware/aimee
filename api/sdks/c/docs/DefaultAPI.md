# DefaultAPI

All URIs are relative to *http://127.0.0.1:8090/v1*

Method | HTTP request | Description
------------- | ------------- | -------------
[**DefaultAPI_deleteDoc**](DefaultAPI.md#DefaultAPI_deleteDoc) | **DELETE** /docs/{id} | Delete a staged document
[**DefaultAPI_getActiveRelease**](DefaultAPI.md#DefaultAPI_getActiveRelease) | **GET** /releases/active | Get the currently active corpus release
[**DefaultAPI_getArtifact**](DefaultAPI.md#DefaultAPI_getArtifact) | **GET** /artifacts/{id} | Retrieve an artifact by UUID
[**DefaultAPI_getArtifactLinks**](DefaultAPI.md#DefaultAPI_getArtifactLinks) | **GET** /artifacts/{id}/links | Retrieve outgoing links from an artifact
[**DefaultAPI_getCapabilities**](DefaultAPI.md#DefaultAPI_getCapabilities) | **GET** /capabilities | Advertised capabilities
[**DefaultAPI_getCodeBlastRadius**](DefaultAPI.md#DefaultAPI_getCodeBlastRadius) | **GET** /code/blast-radius | Blast-radius computation for a file
[**DefaultAPI_getCodeCallers**](DefaultAPI.md#DefaultAPI_getCodeCallers) | **GET** /code/callers | Call sites for a symbol in the canonical code index
[**DefaultAPI_getCodeFind**](DefaultAPI.md#DefaultAPI_getCodeFind) | **GET** /code/find | Symbol/identifier lookup across the canonical index
[**DefaultAPI_getCodeProjectStats**](DefaultAPI.md#DefaultAPI_getCodeProjectStats) | **GET** /code/project-stats | Project-level canonical code index counts and language breakdown
[**DefaultAPI_getCodeProjects**](DefaultAPI.md#DefaultAPI_getCodeProjects) | **GET** /code/projects | List projects in the canonical code index
[**DefaultAPI_getCodeSearch**](DefaultAPI.md#DefaultAPI_getCodeSearch) | **GET** /code/search | Full-text code search across indexed file contents
[**DefaultAPI_getCodeStructure**](DefaultAPI.md#DefaultAPI_getCodeStructure) | **GET** /code/structure | Definitions for a file in the canonical code index
[**DefaultAPI_getDoc**](DefaultAPI.md#DefaultAPI_getDoc) | **GET** /docs/{id} | Retrieve doc metadata by id
[**DefaultAPI_getEntityProfile**](DefaultAPI.md#DefaultAPI_getEntityProfile) | **GET** /entities/{id} | Canonical entity profile
[**DefaultAPI_getHealth**](DefaultAPI.md#DefaultAPI_getHealth) | **GET** /health | Service health check
[**DefaultAPI_getIngestStatus**](DefaultAPI.md#DefaultAPI_getIngestStatus) | **GET** /ingest/status | Report background project ingest status
[**DefaultAPI_getIntelligenceBanditExport**](DefaultAPI.md#DefaultAPI_getIntelligenceBanditExport) | **GET** /intelligence/bandit/export | Export fusion bandit decision data
[**DefaultAPI_getIntelligenceCalibrationReadiness**](DefaultAPI.md#DefaultAPI_getIntelligenceCalibrationReadiness) | **GET** /intelligence/calibration/readiness | Calibration readiness
[**DefaultAPI_getIntelligenceDemotionCheck**](DefaultAPI.md#DefaultAPI_getIntelligenceDemotionCheck) | **GET** /intelligence/demotion/check | Dry-run demotion readiness check
[**DefaultAPI_getJobStatus**](DefaultAPI.md#DefaultAPI_getJobStatus) | **GET** /jobs/{job_id} | Report asynchronous knowledge ingest job status
[**DefaultAPI_getPipelineStatus**](DefaultAPI.md#DefaultAPI_getPipelineStatus) | **GET** /pipeline/status | Report asynchronous knowledge ingest queue status
[**DefaultAPI_getReview**](DefaultAPI.md#DefaultAPI_getReview) | **GET** /review | List staged documents pending review
[**DefaultAPI_getVersion**](DefaultAPI.md#DefaultAPI_getVersion) | **GET** /version | Service version
[**DefaultAPI_getWorkers**](DefaultAPI.md#DefaultAPI_getWorkers) | **GET** /workers | Report aimee-kb worker and background task status
[**DefaultAPI_headHealth**](DefaultAPI.md#DefaultAPI_headHealth) | **HEAD** /health | Service health check (HEAD)
[**DefaultAPI_postAction**](DefaultAPI.md#DefaultAPI_postAction) | **POST** /actions/{action} | Execute a versioned knowledge-service action
[**DefaultAPI_postCodeBuild**](DefaultAPI.md#DefaultAPI_postCodeBuild) | **POST** /code/build | Build a project knowledge index
[**DefaultAPI_postCodeScan**](DefaultAPI.md#DefaultAPI_postCodeScan) | **POST** /code/scan | Request a canonical code index scan
[**DefaultAPI_postCodeUpdate**](DefaultAPI.md#DefaultAPI_postCodeUpdate) | **POST** /code/update | Incrementally update a project knowledge index
[**DefaultAPI_postDocs**](DefaultAPI.md#DefaultAPI_postDocs) | **POST** /docs | Upload a document for ingest
[**DefaultAPI_postDocsManifest**](DefaultAPI.md#DefaultAPI_postDocsManifest) | **POST** /docs/manifest | Check which uploaded document hashes are absent
[**DefaultAPI_postDrain**](DefaultAPI.md#DefaultAPI_postDrain) | **POST** /drain | Drain the asynchronous knowledge ingest queue
[**DefaultAPI_postEntitySearch**](DefaultAPI.md#DefaultAPI_postEntitySearch) | **POST** /entities/search | Find entities by name or context
[**DefaultAPI_postIngest**](DefaultAPI.md#DefaultAPI_postIngest) | **POST** /ingest | Enqueue background project ingest
[**DefaultAPI_postIntelligenceBanditClose**](DefaultAPI.md#DefaultAPI_postIntelligenceBanditClose) | **POST** /intelligence/bandit/close | Close a sampled decision with its observed reward
[**DefaultAPI_postIntelligenceBanditPromote**](DefaultAPI.md#DefaultAPI_postIntelligenceBanditPromote) | **POST** /intelligence/bandit/promote | Persist the production-default arm for a decision point
[**DefaultAPI_postIntelligenceBanditSample**](DefaultAPI.md#DefaultAPI_postIntelligenceBanditSample) | **POST** /intelligence/bandit/sample | Sample an arm for a decision point (server-side decision points)
[**DefaultAPI_postMaintenanceClear**](DefaultAPI.md#DefaultAPI_postMaintenanceClear) | **POST** /maintenance/clear | Clear indexed knowledge for a project
[**DefaultAPI_postMaintenanceReconcile**](DefaultAPI.md#DefaultAPI_postMaintenanceReconcile) | **POST** /maintenance/reconcile | Reconcile orphaned vector records
[**DefaultAPI_postMaintenanceRepair**](DefaultAPI.md#DefaultAPI_postMaintenanceRepair) | **POST** /maintenance/repair | Repair a project knowledge index
[**DefaultAPI_postPromote**](DefaultAPI.md#DefaultAPI_postPromote) | **POST** /releases/{id}/promote | Promote a release to active
[**DefaultAPI_postReleases**](DefaultAPI.md#DefaultAPI_postReleases) | **POST** /releases | Create a new corpus release
[**DefaultAPI_postReviewAccept**](DefaultAPI.md#DefaultAPI_postReviewAccept) | **POST** /review/{id}/accept | Accept a staged document
[**DefaultAPI_postReviewReject**](DefaultAPI.md#DefaultAPI_postReviewReject) | **POST** /review/{id}/reject | Reject a staged document
[**DefaultAPI_postRollback**](DefaultAPI.md#DefaultAPI_postRollback) | **POST** /releases/{id}/rollback | Roll back to a prior release
[**DefaultAPI_postSearch**](DefaultAPI.md#DefaultAPI_postSearch) | **POST** /search | Hybrid knowledge search


# **DefaultAPI_deleteDoc**
```c
// Delete a staged document
//
void DefaultAPI_deleteDoc(apiClient_t *apiClient, long id);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**id** | **long** |  | 

### Return type

void

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: Not defined

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_getActiveRelease**
```c
// Get the currently active corpus release
//
active_release_response_t* DefaultAPI_getActiveRelease(apiClient_t *apiClient);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |

### Return type

[active_release_response_t](active_release_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_getArtifact**
```c
// Retrieve an artifact by UUID
//
artifact_response_t* DefaultAPI_getArtifact(apiClient_t *apiClient, char *id);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**id** | **char \*** |  | 

### Return type

[artifact_response_t](artifact_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_getArtifactLinks**
```c
// Retrieve outgoing links from an artifact
//
artifact_links_response_t* DefaultAPI_getArtifactLinks(apiClient_t *apiClient, char *id);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**id** | **char \*** |  | 

### Return type

[artifact_links_response_t](artifact_links_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_getCapabilities**
```c
// Advertised capabilities
//
// Returns the set of capability strings this aimee-kb instance supports. Phase 1 always returns [\"memory\", \"search\", \"index\"]. 
//
capabilities_response_t* DefaultAPI_getCapabilities(apiClient_t *apiClient);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |

### Return type

[capabilities_response_t](capabilities_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_getCodeBlastRadius**
```c
// Blast-radius computation for a file
//
blast_radius_response_t* DefaultAPI_getCodeBlastRadius(apiClient_t *apiClient, char *project, char *file_path);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**project** | **char \*** |  | 
**file_path** | **char \*** |  | [optional] 

### Return type

[blast_radius_response_t](blast_radius_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_getCodeCallers**
```c
// Call sites for a symbol in the canonical code index
//
code_callers_response_t* DefaultAPI_getCodeCallers(apiClient_t *apiClient, char *symbol, char *project, int *max_results);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**symbol** | **char \*** |  | 
**project** | **char \*** |  | [optional] 
**max_results** | **int \*** |  | [optional] [default to 20]

### Return type

[code_callers_response_t](code_callers_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_getCodeFind**
```c
// Symbol/identifier lookup across the canonical index
//
code_find_response_t* DefaultAPI_getCodeFind(apiClient_t *apiClient, char *identifier, char *project, int *max_results);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**identifier** | **char \*** |  | 
**project** | **char \*** |  | [optional] 
**max_results** | **int \*** |  | [optional] [default to 20]

### Return type

[code_find_response_t](code_find_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_getCodeProjectStats**
```c
// Project-level canonical code index counts and language breakdown
//
code_project_stats_response_t* DefaultAPI_getCodeProjectStats(apiClient_t *apiClient, char *project);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**project** | **char \*** |  | 

### Return type

[code_project_stats_response_t](code_project_stats_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_getCodeProjects**
```c
// List projects in the canonical code index
//
code_projects_response_t* DefaultAPI_getCodeProjects(apiClient_t *apiClient, int *max_results);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**max_results** | **int \*** |  | [optional] [default to 100]

### Return type

[code_projects_response_t](code_projects_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_getCodeSearch**
```c
// Full-text code search across indexed file contents
//
code_search_response_t* DefaultAPI_getCodeSearch(apiClient_t *apiClient, char *query, char *project, int *max_results);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**query** | **char \*** |  | 
**project** | **char \*** |  | [optional] 
**max_results** | **int \*** |  | [optional] [default to 20]

### Return type

[code_search_response_t](code_search_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_getCodeStructure**
```c
// Definitions for a file in the canonical code index
//
code_structure_response_t* DefaultAPI_getCodeStructure(apiClient_t *apiClient, char *project, char *file_path, int *max_results);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**project** | **char \*** |  | 
**file_path** | **char \*** |  | 
**max_results** | **int \*** |  | [optional] [default to 256]

### Return type

[code_structure_response_t](code_structure_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_getDoc**
```c
// Retrieve doc metadata by id
//
doc_metadata_response_t* DefaultAPI_getDoc(apiClient_t *apiClient, long id);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**id** | **long** |  | 

### Return type

[doc_metadata_response_t](doc_metadata_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_getEntityProfile**
```c
// Canonical entity profile
//
entity_profile_response_t* DefaultAPI_getEntityProfile(apiClient_t *apiClient, char *id);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**id** | **char \*** | Entity name (slug) | 

### Return type

[entity_profile_response_t](entity_profile_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_getHealth**
```c
// Service health check
//
// Returns {\"status\":\"ok\"} when the service is running.
//
health_response_t* DefaultAPI_getHealth(apiClient_t *apiClient);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |

### Return type

[health_response_t](health_response.md) *


### Authorization

No authorization required

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_getIngestStatus**
```c
// Report background project ingest status
//
ingest_status_response_t* DefaultAPI_getIngestStatus(apiClient_t *apiClient);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |

### Return type

[ingest_status_response_t](ingest_status_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_getIntelligenceBanditExport**
```c
// Export fusion bandit decision data
//
object_t* DefaultAPI_getIntelligenceBanditExport(apiClient_t *apiClient);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |

### Return type

[object_t](object.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_getIntelligenceCalibrationReadiness**
```c
// Calibration readiness
//
object_t* DefaultAPI_getIntelligenceCalibrationReadiness(apiClient_t *apiClient);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |

### Return type

[object_t](object.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_getIntelligenceDemotionCheck**
```c
// Dry-run demotion readiness check
//
object_t* DefaultAPI_getIntelligenceDemotionCheck(apiClient_t *apiClient);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |

### Return type

[object_t](object.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_getJobStatus**
```c
// Report asynchronous knowledge ingest job status
//
job_status_response_t* DefaultAPI_getJobStatus(apiClient_t *apiClient, long job_id);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**job_id** | **long** |  | 

### Return type

[job_status_response_t](job_status_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_getPipelineStatus**
```c
// Report asynchronous knowledge ingest queue status
//
pipeline_status_response_t* DefaultAPI_getPipelineStatus(apiClient_t *apiClient);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |

### Return type

[pipeline_status_response_t](pipeline_status_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_getReview**
```c
// List staged documents pending review
//
review_queue_response_t* DefaultAPI_getReview(apiClient_t *apiClient, long cursor, int *limit);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**cursor** | **long** |  | [optional] 
**limit** | **int \*** |  | [optional] [default to 10]

### Return type

[review_queue_response_t](review_queue_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_getVersion**
```c
// Service version
//
version_response_t* DefaultAPI_getVersion(apiClient_t *apiClient);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |

### Return type

[version_response_t](version_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_getWorkers**
```c
// Report aimee-kb worker and background task status
//
workers_response_t* DefaultAPI_getWorkers(apiClient_t *apiClient);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |

### Return type

[workers_response_t](workers_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_headHealth**
```c
// Service health check (HEAD)
//
void DefaultAPI_headHealth(apiClient_t *apiClient);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |

### Return type

void

### Authorization

No authorization required

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: Not defined

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_postAction**
```c
// Execute a versioned knowledge-service action
//
object_t* DefaultAPI_postAction(apiClient_t *apiClient, char *action, object_t *body);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**action** | **char \*** |  | 
**body** | **[object_t](object.md) \*** |  | [optional] 

### Return type

[object_t](object.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_postCodeBuild**
```c
// Build a project knowledge index
//
code_build_response_t* DefaultAPI_postCodeBuild(apiClient_t *apiClient, code_build_request_t *code_build_request);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**code_build_request** | **[code_build_request_t](code_build_request.md) \*** |  | 

### Return type

[code_build_response_t](code_build_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_postCodeScan**
```c
// Request a canonical code index scan
//
code_scan_response_t* DefaultAPI_postCodeScan(apiClient_t *apiClient, code_scan_request_t *code_scan_request);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**code_scan_request** | **[code_scan_request_t](code_scan_request.md) \*** |  | 

### Return type

[code_scan_response_t](code_scan_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_postCodeUpdate**
```c
// Incrementally update a project knowledge index
//
code_update_response_t* DefaultAPI_postCodeUpdate(apiClient_t *apiClient, code_update_request_t *code_update_request);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**code_update_request** | **[code_update_request_t](code_update_request.md) \*** |  | 

### Return type

[code_update_response_t](code_update_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_postDocs**
```c
// Upload a document for ingest
//
// Accepts multipart/form-data with a required `file` part and an optional `scope` field (default: global). Normalizes to markdown, stores in DB2, and queues an extract_doc job. Returns existing doc_id on idempotent re-upload. 
//
doc_ingest_response_t* DefaultAPI_postDocs(apiClient_t *apiClient, binary_t* file, char *scope);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**file** | **binary_t*** |  | 
**scope** | **char \*** |  | [optional] [default to &#39;global&#39;]

### Return type

[doc_ingest_response_t](doc_ingest_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: multipart/form-data
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_postDocsManifest**
```c
// Check which uploaded document hashes are absent
//
docs_manifest_response_t* DefaultAPI_postDocsManifest(apiClient_t *apiClient, docs_manifest_request_t *docs_manifest_request);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**docs_manifest_request** | **[docs_manifest_request_t](docs_manifest_request.md) \*** |  | 

### Return type

[docs_manifest_response_t](docs_manifest_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_postDrain**
```c
// Drain the asynchronous knowledge ingest queue
//
drain_response_t* DefaultAPI_postDrain(apiClient_t *apiClient, drain_request_t *drain_request);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**drain_request** | **[drain_request_t](drain_request.md) \*** |  | [optional] 

### Return type

[drain_response_t](drain_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_postEntitySearch**
```c
// Find entities by name or context
//
entity_search_response_t* DefaultAPI_postEntitySearch(apiClient_t *apiClient, entity_search_request_t *entity_search_request);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**entity_search_request** | **[entity_search_request_t](entity_search_request.md) \*** |  | 

### Return type

[entity_search_response_t](entity_search_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_postIngest**
```c
// Enqueue background project ingest
//
ingest_response_t* DefaultAPI_postIngest(apiClient_t *apiClient, ingest_request_t *ingest_request);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**ingest_request** | **[ingest_request_t](ingest_request.md) \*** |  | [optional] 

### Return type

[ingest_response_t](ingest_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_postIntelligenceBanditClose**
```c
// Close a sampled decision with its observed reward
//
object_t* DefaultAPI_postIntelligenceBanditClose(apiClient_t *apiClient);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |

### Return type

[object_t](object.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_postIntelligenceBanditPromote**
```c
// Persist the production-default arm for a decision point
//
object_t* DefaultAPI_postIntelligenceBanditPromote(apiClient_t *apiClient);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |

### Return type

[object_t](object.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_postIntelligenceBanditSample**
```c
// Sample an arm for a decision point (server-side decision points)
//
object_t* DefaultAPI_postIntelligenceBanditSample(apiClient_t *apiClient);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |

### Return type

[object_t](object.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_postMaintenanceClear**
```c
// Clear indexed knowledge for a project
//
maintenance_clear_response_t* DefaultAPI_postMaintenanceClear(apiClient_t *apiClient, maintenance_clear_request_t *maintenance_clear_request);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**maintenance_clear_request** | **[maintenance_clear_request_t](maintenance_clear_request.md) \*** |  | 

### Return type

[maintenance_clear_response_t](maintenance_clear_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_postMaintenanceReconcile**
```c
// Reconcile orphaned vector records
//
maintenance_reconcile_response_t* DefaultAPI_postMaintenanceReconcile(apiClient_t *apiClient, maintenance_reconcile_request_t *maintenance_reconcile_request);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**maintenance_reconcile_request** | **[maintenance_reconcile_request_t](maintenance_reconcile_request.md) \*** |  | [optional] 

### Return type

[maintenance_reconcile_response_t](maintenance_reconcile_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_postMaintenanceRepair**
```c
// Repair a project knowledge index
//
maintenance_repair_response_t* DefaultAPI_postMaintenanceRepair(apiClient_t *apiClient, maintenance_repair_request_t *maintenance_repair_request);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**maintenance_repair_request** | **[maintenance_repair_request_t](maintenance_repair_request.md) \*** |  | 

### Return type

[maintenance_repair_response_t](maintenance_repair_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_postPromote**
```c
// Promote a release to active
//
void DefaultAPI_postPromote(apiClient_t *apiClient, long id);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**id** | **long** |  | 

### Return type

void

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: Not defined

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_postReleases**
```c
// Create a new corpus release
//
create_release_response_t* DefaultAPI_postReleases(apiClient_t *apiClient, create_release_request_t *create_release_request);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**create_release_request** | **[create_release_request_t](create_release_request.md) \*** |  | 

### Return type

[create_release_response_t](create_release_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_postReviewAccept**
```c
// Accept a staged document
//
void DefaultAPI_postReviewAccept(apiClient_t *apiClient, long id, review_accept_request_t *review_accept_request);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**id** | **long** |  | 
**review_accept_request** | **[review_accept_request_t](review_accept_request.md) \*** |  | [optional] 

### Return type

void

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: Not defined

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_postReviewReject**
```c
// Reject a staged document
//
void DefaultAPI_postReviewReject(apiClient_t *apiClient, long id, review_reject_request_t *review_reject_request);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**id** | **long** |  | 
**review_reject_request** | **[review_reject_request_t](review_reject_request.md) \*** |  | 

### Return type

void

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: Not defined

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_postRollback**
```c
// Roll back to a prior release
//
void DefaultAPI_postRollback(apiClient_t *apiClient, long id, rollback_request_t *rollback_request);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**id** | **long** |  | 
**rollback_request** | **[rollback_request_t](rollback_request.md) \*** |  | [optional] 

### Return type

void

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: Not defined

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **DefaultAPI_postSearch**
```c
// Hybrid knowledge search
//
search_response_t* DefaultAPI_postSearch(apiClient_t *apiClient, search_request_t *search_request);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**search_request** | **[search_request_t](search_request.md) \*** |  | 

### Return type

[search_response_t](search_response.md) *


### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

