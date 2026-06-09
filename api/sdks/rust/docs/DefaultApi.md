# \DefaultApi

All URIs are relative to *http://127.0.0.1:8090/v1*

Method | HTTP request | Description
------------- | ------------- | -------------
[**delete_doc**](DefaultApi.md#delete_doc) | **DELETE** /docs/{id} | Delete a staged document
[**get_active_release**](DefaultApi.md#get_active_release) | **GET** /releases/active | Get the currently active corpus release
[**get_artifact**](DefaultApi.md#get_artifact) | **GET** /artifacts/{id} | Retrieve an artifact by UUID
[**get_artifact_links**](DefaultApi.md#get_artifact_links) | **GET** /artifacts/{id}/links | Retrieve outgoing links from an artifact
[**get_capabilities**](DefaultApi.md#get_capabilities) | **GET** /capabilities | Advertised capabilities
[**get_code_blast_radius**](DefaultApi.md#get_code_blast_radius) | **GET** /code/blast-radius | Blast-radius computation for a file
[**get_code_callers**](DefaultApi.md#get_code_callers) | **GET** /code/callers | Call sites for a symbol in the canonical code index
[**get_code_find**](DefaultApi.md#get_code_find) | **GET** /code/find | Symbol/identifier lookup across the canonical index
[**get_code_project_stats**](DefaultApi.md#get_code_project_stats) | **GET** /code/project-stats | Project-level canonical code index counts and language breakdown
[**get_code_projects**](DefaultApi.md#get_code_projects) | **GET** /code/projects | List projects in the canonical code index
[**get_code_search**](DefaultApi.md#get_code_search) | **GET** /code/search | Full-text code search across indexed file contents
[**get_code_structure**](DefaultApi.md#get_code_structure) | **GET** /code/structure | Definitions for a file in the canonical code index
[**get_doc**](DefaultApi.md#get_doc) | **GET** /docs/{id} | Retrieve doc metadata by id
[**get_entity_profile**](DefaultApi.md#get_entity_profile) | **GET** /entities/{id} | Canonical entity profile
[**get_health**](DefaultApi.md#get_health) | **GET** /health | Service health check
[**get_ingest_status**](DefaultApi.md#get_ingest_status) | **GET** /ingest/status | Report background project ingest status
[**get_intelligence_bandit_export**](DefaultApi.md#get_intelligence_bandit_export) | **GET** /intelligence/bandit/export | Export fusion bandit decision data
[**get_intelligence_calibration_readiness**](DefaultApi.md#get_intelligence_calibration_readiness) | **GET** /intelligence/calibration/readiness | Calibration readiness
[**get_intelligence_demotion_check**](DefaultApi.md#get_intelligence_demotion_check) | **GET** /intelligence/demotion/check | Dry-run demotion readiness check
[**get_job_status**](DefaultApi.md#get_job_status) | **GET** /jobs/{job_id} | Report asynchronous knowledge ingest job status
[**get_pipeline_status**](DefaultApi.md#get_pipeline_status) | **GET** /pipeline/status | Report asynchronous knowledge ingest queue status
[**get_review**](DefaultApi.md#get_review) | **GET** /review | List staged documents pending review
[**get_version**](DefaultApi.md#get_version) | **GET** /version | Service version
[**get_workers**](DefaultApi.md#get_workers) | **GET** /workers | Report aimee-kb worker and background task status
[**head_health**](DefaultApi.md#head_health) | **HEAD** /health | Service health check (HEAD)
[**post_action**](DefaultApi.md#post_action) | **POST** /actions/{action} | Execute a versioned knowledge-service action
[**post_code_build**](DefaultApi.md#post_code_build) | **POST** /code/build | Build a project knowledge index
[**post_code_scan**](DefaultApi.md#post_code_scan) | **POST** /code/scan | Request a canonical code index scan
[**post_code_update**](DefaultApi.md#post_code_update) | **POST** /code/update | Incrementally update a project knowledge index
[**post_docs**](DefaultApi.md#post_docs) | **POST** /docs | Upload a document for ingest
[**post_docs_manifest**](DefaultApi.md#post_docs_manifest) | **POST** /docs/manifest | Check which uploaded document hashes are absent
[**post_drain**](DefaultApi.md#post_drain) | **POST** /drain | Drain the asynchronous knowledge ingest queue
[**post_entity_search**](DefaultApi.md#post_entity_search) | **POST** /entities/search | Find entities by name or context
[**post_ingest**](DefaultApi.md#post_ingest) | **POST** /ingest | Enqueue background project ingest
[**post_intelligence_bandit_close**](DefaultApi.md#post_intelligence_bandit_close) | **POST** /intelligence/bandit/close | Close a sampled decision with its observed reward
[**post_intelligence_bandit_promote**](DefaultApi.md#post_intelligence_bandit_promote) | **POST** /intelligence/bandit/promote | Persist the production-default arm for a decision point
[**post_intelligence_bandit_sample**](DefaultApi.md#post_intelligence_bandit_sample) | **POST** /intelligence/bandit/sample | Sample an arm for a decision point (server-side decision points)
[**post_maintenance_clear**](DefaultApi.md#post_maintenance_clear) | **POST** /maintenance/clear | Clear indexed knowledge for a project
[**post_maintenance_reconcile**](DefaultApi.md#post_maintenance_reconcile) | **POST** /maintenance/reconcile | Reconcile orphaned vector records
[**post_maintenance_repair**](DefaultApi.md#post_maintenance_repair) | **POST** /maintenance/repair | Repair a project knowledge index
[**post_promote**](DefaultApi.md#post_promote) | **POST** /releases/{id}/promote | Promote a release to active
[**post_releases**](DefaultApi.md#post_releases) | **POST** /releases | Create a new corpus release
[**post_review_accept**](DefaultApi.md#post_review_accept) | **POST** /review/{id}/accept | Accept a staged document
[**post_review_reject**](DefaultApi.md#post_review_reject) | **POST** /review/{id}/reject | Reject a staged document
[**post_rollback**](DefaultApi.md#post_rollback) | **POST** /releases/{id}/rollback | Roll back to a prior release
[**post_search**](DefaultApi.md#post_search) | **POST** /search | Hybrid knowledge search



## delete_doc

> delete_doc(id)
Delete a staged document

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**id** | **i64** |  | [required] |

### Return type

 (empty response body)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: Not defined

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## get_active_release

> models::ActiveReleaseResponse get_active_release()
Get the currently active corpus release

### Parameters

This endpoint does not need any parameter.

### Return type

[**models::ActiveReleaseResponse**](ActiveReleaseResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## get_artifact

> models::ArtifactResponse get_artifact(id)
Retrieve an artifact by UUID

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**id** | **String** |  | [required] |

### Return type

[**models::ArtifactResponse**](ArtifactResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## get_artifact_links

> models::ArtifactLinksResponse get_artifact_links(id)
Retrieve outgoing links from an artifact

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**id** | **String** |  | [required] |

### Return type

[**models::ArtifactLinksResponse**](ArtifactLinksResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## get_capabilities

> models::CapabilitiesResponse get_capabilities()
Advertised capabilities

Returns the set of capability strings this aimee-kb instance supports. Phase 1 always returns [\"memory\", \"search\", \"index\"]. 

### Parameters

This endpoint does not need any parameter.

### Return type

[**models::CapabilitiesResponse**](CapabilitiesResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## get_code_blast_radius

> models::BlastRadiusResponse get_code_blast_radius(project, file_path)
Blast-radius computation for a file

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**project** | **String** |  | [required] |
**file_path** | Option<**String**> |  |  |

### Return type

[**models::BlastRadiusResponse**](BlastRadiusResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## get_code_callers

> models::CodeCallersResponse get_code_callers(symbol, project, max_results)
Call sites for a symbol in the canonical code index

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**symbol** | **String** |  | [required] |
**project** | Option<**String**> |  |  |
**max_results** | Option<**i32**> |  |  |[default to 20]

### Return type

[**models::CodeCallersResponse**](CodeCallersResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## get_code_find

> models::CodeFindResponse get_code_find(identifier, project, max_results)
Symbol/identifier lookup across the canonical index

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**identifier** | **String** |  | [required] |
**project** | Option<**String**> |  |  |
**max_results** | Option<**i32**> |  |  |[default to 20]

### Return type

[**models::CodeFindResponse**](CodeFindResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## get_code_project_stats

> models::CodeProjectStatsResponse get_code_project_stats(project)
Project-level canonical code index counts and language breakdown

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**project** | **String** |  | [required] |

### Return type

[**models::CodeProjectStatsResponse**](CodeProjectStatsResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## get_code_projects

> models::CodeProjectsResponse get_code_projects(max_results)
List projects in the canonical code index

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**max_results** | Option<**i32**> |  |  |[default to 100]

### Return type

[**models::CodeProjectsResponse**](CodeProjectsResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## get_code_search

> models::CodeSearchResponse get_code_search(query, project, max_results)
Full-text code search across indexed file contents

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**query** | **String** |  | [required] |
**project** | Option<**String**> |  |  |
**max_results** | Option<**i32**> |  |  |[default to 20]

### Return type

[**models::CodeSearchResponse**](CodeSearchResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## get_code_structure

> models::CodeStructureResponse get_code_structure(project, file_path, max_results)
Definitions for a file in the canonical code index

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**project** | **String** |  | [required] |
**file_path** | **String** |  | [required] |
**max_results** | Option<**i32**> |  |  |[default to 256]

### Return type

[**models::CodeStructureResponse**](CodeStructureResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## get_doc

> models::DocMetadataResponse get_doc(id)
Retrieve doc metadata by id

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**id** | **i64** |  | [required] |

### Return type

[**models::DocMetadataResponse**](DocMetadataResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## get_entity_profile

> models::EntityProfileResponse get_entity_profile(id)
Canonical entity profile

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**id** | **String** | Entity name (slug) | [required] |

### Return type

[**models::EntityProfileResponse**](EntityProfileResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## get_health

> models::HealthResponse get_health()
Service health check

Returns {\"status\":\"ok\"} when the service is running.

### Parameters

This endpoint does not need any parameter.

### Return type

[**models::HealthResponse**](HealthResponse.md)

### Authorization

No authorization required

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## get_ingest_status

> models::IngestStatusResponse get_ingest_status()
Report background project ingest status

### Parameters

This endpoint does not need any parameter.

### Return type

[**models::IngestStatusResponse**](IngestStatusResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## get_intelligence_bandit_export

> serde_json::Value get_intelligence_bandit_export()
Export fusion bandit decision data

### Parameters

This endpoint does not need any parameter.

### Return type

[**serde_json::Value**](serde_json::Value.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## get_intelligence_calibration_readiness

> serde_json::Value get_intelligence_calibration_readiness()
Calibration readiness

### Parameters

This endpoint does not need any parameter.

### Return type

[**serde_json::Value**](serde_json::Value.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## get_intelligence_demotion_check

> serde_json::Value get_intelligence_demotion_check()
Dry-run demotion readiness check

### Parameters

This endpoint does not need any parameter.

### Return type

[**serde_json::Value**](serde_json::Value.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## get_job_status

> models::JobStatusResponse get_job_status(job_id)
Report asynchronous knowledge ingest job status

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**job_id** | **i64** |  | [required] |

### Return type

[**models::JobStatusResponse**](JobStatusResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## get_pipeline_status

> models::PipelineStatusResponse get_pipeline_status()
Report asynchronous knowledge ingest queue status

### Parameters

This endpoint does not need any parameter.

### Return type

[**models::PipelineStatusResponse**](PipelineStatusResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## get_review

> models::ReviewQueueResponse get_review(cursor, limit)
List staged documents pending review

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**cursor** | Option<**i64**> |  |  |
**limit** | Option<**i32**> |  |  |[default to 10]

### Return type

[**models::ReviewQueueResponse**](ReviewQueueResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## get_version

> models::VersionResponse get_version()
Service version

### Parameters

This endpoint does not need any parameter.

### Return type

[**models::VersionResponse**](VersionResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## get_workers

> models::WorkersResponse get_workers()
Report aimee-kb worker and background task status

### Parameters

This endpoint does not need any parameter.

### Return type

[**models::WorkersResponse**](WorkersResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## head_health

> head_health()
Service health check (HEAD)

### Parameters

This endpoint does not need any parameter.

### Return type

 (empty response body)

### Authorization

No authorization required

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: Not defined

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## post_action

> serde_json::Value post_action(action, body)
Execute a versioned knowledge-service action

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**action** | **String** |  | [required] |
**body** | Option<**serde_json::Value**> |  |  |

### Return type

[**serde_json::Value**](serde_json::Value.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## post_code_build

> models::CodeBuildResponse post_code_build(code_build_request)
Build a project knowledge index

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**code_build_request** | [**CodeBuildRequest**](CodeBuildRequest.md) |  | [required] |

### Return type

[**models::CodeBuildResponse**](CodeBuildResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## post_code_scan

> models::CodeScanResponse post_code_scan(code_scan_request)
Request a canonical code index scan

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**code_scan_request** | [**CodeScanRequest**](CodeScanRequest.md) |  | [required] |

### Return type

[**models::CodeScanResponse**](CodeScanResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## post_code_update

> models::CodeUpdateResponse post_code_update(code_update_request)
Incrementally update a project knowledge index

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**code_update_request** | [**CodeUpdateRequest**](CodeUpdateRequest.md) |  | [required] |

### Return type

[**models::CodeUpdateResponse**](CodeUpdateResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## post_docs

> models::DocIngestResponse post_docs(file, scope)
Upload a document for ingest

Accepts multipart/form-data with a required `file` part and an optional `scope` field (default: global). Normalizes to markdown, stores in DB2, and queues an extract_doc job. Returns existing doc_id on idempotent re-upload. 

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**file** | **std::path::PathBuf** |  | [required] |
**scope** | Option<**String**> |  |  |[default to global]

### Return type

[**models::DocIngestResponse**](DocIngestResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: multipart/form-data
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## post_docs_manifest

> models::DocsManifestResponse post_docs_manifest(docs_manifest_request)
Check which uploaded document hashes are absent

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**docs_manifest_request** | [**DocsManifestRequest**](DocsManifestRequest.md) |  | [required] |

### Return type

[**models::DocsManifestResponse**](DocsManifestResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## post_drain

> models::DrainResponse post_drain(drain_request)
Drain the asynchronous knowledge ingest queue

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**drain_request** | Option<[**DrainRequest**](DrainRequest.md)> |  |  |

### Return type

[**models::DrainResponse**](DrainResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## post_entity_search

> models::EntitySearchResponse post_entity_search(entity_search_request)
Find entities by name or context

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**entity_search_request** | [**EntitySearchRequest**](EntitySearchRequest.md) |  | [required] |

### Return type

[**models::EntitySearchResponse**](EntitySearchResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## post_ingest

> models::IngestResponse post_ingest(ingest_request)
Enqueue background project ingest

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**ingest_request** | Option<[**IngestRequest**](IngestRequest.md)> |  |  |

### Return type

[**models::IngestResponse**](IngestResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## post_intelligence_bandit_close

> serde_json::Value post_intelligence_bandit_close()
Close a sampled decision with its observed reward

### Parameters

This endpoint does not need any parameter.

### Return type

[**serde_json::Value**](serde_json::Value.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## post_intelligence_bandit_promote

> serde_json::Value post_intelligence_bandit_promote()
Persist the production-default arm for a decision point

### Parameters

This endpoint does not need any parameter.

### Return type

[**serde_json::Value**](serde_json::Value.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## post_intelligence_bandit_sample

> serde_json::Value post_intelligence_bandit_sample()
Sample an arm for a decision point (server-side decision points)

### Parameters

This endpoint does not need any parameter.

### Return type

[**serde_json::Value**](serde_json::Value.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## post_maintenance_clear

> models::MaintenanceClearResponse post_maintenance_clear(maintenance_clear_request)
Clear indexed knowledge for a project

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**maintenance_clear_request** | [**MaintenanceClearRequest**](MaintenanceClearRequest.md) |  | [required] |

### Return type

[**models::MaintenanceClearResponse**](MaintenanceClearResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## post_maintenance_reconcile

> models::MaintenanceReconcileResponse post_maintenance_reconcile(maintenance_reconcile_request)
Reconcile orphaned vector records

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**maintenance_reconcile_request** | Option<[**MaintenanceReconcileRequest**](MaintenanceReconcileRequest.md)> |  |  |

### Return type

[**models::MaintenanceReconcileResponse**](MaintenanceReconcileResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## post_maintenance_repair

> models::MaintenanceRepairResponse post_maintenance_repair(maintenance_repair_request)
Repair a project knowledge index

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**maintenance_repair_request** | [**MaintenanceRepairRequest**](MaintenanceRepairRequest.md) |  | [required] |

### Return type

[**models::MaintenanceRepairResponse**](MaintenanceRepairResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## post_promote

> post_promote(id)
Promote a release to active

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**id** | **i64** |  | [required] |

### Return type

 (empty response body)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: Not defined

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## post_releases

> models::CreateReleaseResponse post_releases(create_release_request)
Create a new corpus release

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**create_release_request** | [**CreateReleaseRequest**](CreateReleaseRequest.md) |  | [required] |

### Return type

[**models::CreateReleaseResponse**](CreateReleaseResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## post_review_accept

> post_review_accept(id, review_accept_request)
Accept a staged document

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**id** | **i64** |  | [required] |
**review_accept_request** | Option<[**ReviewAcceptRequest**](ReviewAcceptRequest.md)> |  |  |

### Return type

 (empty response body)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: Not defined

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## post_review_reject

> post_review_reject(id, review_reject_request)
Reject a staged document

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**id** | **i64** |  | [required] |
**review_reject_request** | [**ReviewRejectRequest**](ReviewRejectRequest.md) |  | [required] |

### Return type

 (empty response body)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: Not defined

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## post_rollback

> post_rollback(id, rollback_request)
Roll back to a prior release

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**id** | **i64** |  | [required] |
**rollback_request** | Option<[**RollbackRequest**](RollbackRequest.md)> |  |  |

### Return type

 (empty response body)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: Not defined

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


## post_search

> models::SearchResponse post_search(search_request)
Hybrid knowledge search

### Parameters


Name | Type | Description  | Required | Notes
------------- | ------------- | ------------- | ------------- | -------------
**search_request** | [**SearchRequest**](SearchRequest.md) |  | [required] |

### Return type

[**models::SearchResponse**](SearchResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

