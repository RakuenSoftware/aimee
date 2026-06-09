# AimeeKb.Api.DefaultApi

All URIs are relative to *http://127.0.0.1:8090/v1*

| Method | HTTP request | Description |
|--------|--------------|-------------|
| [**DeleteDoc**](DefaultApi.md#deletedoc) | **DELETE** /docs/{id} | Delete a staged document |
| [**GetActiveRelease**](DefaultApi.md#getactiverelease) | **GET** /releases/active | Get the currently active corpus release |
| [**GetArtifact**](DefaultApi.md#getartifact) | **GET** /artifacts/{id} | Retrieve an artifact by UUID |
| [**GetArtifactLinks**](DefaultApi.md#getartifactlinks) | **GET** /artifacts/{id}/links | Retrieve outgoing links from an artifact |
| [**GetCapabilities**](DefaultApi.md#getcapabilities) | **GET** /capabilities | Advertised capabilities |
| [**GetCodeBlastRadius**](DefaultApi.md#getcodeblastradius) | **GET** /code/blast-radius | Blast-radius computation for a file |
| [**GetCodeCallers**](DefaultApi.md#getcodecallers) | **GET** /code/callers | Call sites for a symbol in the canonical code index |
| [**GetCodeFind**](DefaultApi.md#getcodefind) | **GET** /code/find | Symbol/identifier lookup across the canonical index |
| [**GetCodeProjectStats**](DefaultApi.md#getcodeprojectstats) | **GET** /code/project-stats | Project-level canonical code index counts and language breakdown |
| [**GetCodeProjects**](DefaultApi.md#getcodeprojects) | **GET** /code/projects | List projects in the canonical code index |
| [**GetCodeSearch**](DefaultApi.md#getcodesearch) | **GET** /code/search | Full-text code search across indexed file contents |
| [**GetCodeStructure**](DefaultApi.md#getcodestructure) | **GET** /code/structure | Definitions for a file in the canonical code index |
| [**GetDoc**](DefaultApi.md#getdoc) | **GET** /docs/{id} | Retrieve doc metadata by id |
| [**GetEntityProfile**](DefaultApi.md#getentityprofile) | **GET** /entities/{id} | Canonical entity profile |
| [**GetHealth**](DefaultApi.md#gethealth) | **GET** /health | Service health check |
| [**GetIngestStatus**](DefaultApi.md#getingeststatus) | **GET** /ingest/status | Report background project ingest status |
| [**GetIntelligenceBanditExport**](DefaultApi.md#getintelligencebanditexport) | **GET** /intelligence/bandit/export | Export fusion bandit decision data |
| [**GetIntelligenceCalibrationReadiness**](DefaultApi.md#getintelligencecalibrationreadiness) | **GET** /intelligence/calibration/readiness | Calibration readiness |
| [**GetIntelligenceDemotionCheck**](DefaultApi.md#getintelligencedemotioncheck) | **GET** /intelligence/demotion/check | Dry-run demotion readiness check |
| [**GetJobStatus**](DefaultApi.md#getjobstatus) | **GET** /jobs/{job_id} | Report asynchronous knowledge ingest job status |
| [**GetPipelineStatus**](DefaultApi.md#getpipelinestatus) | **GET** /pipeline/status | Report asynchronous knowledge ingest queue status |
| [**GetReview**](DefaultApi.md#getreview) | **GET** /review | List staged documents pending review |
| [**GetVersion**](DefaultApi.md#getversion) | **GET** /version | Service version |
| [**GetWorkers**](DefaultApi.md#getworkers) | **GET** /workers | Report aimee-kb worker and background task status |
| [**HeadHealth**](DefaultApi.md#headhealth) | **HEAD** /health | Service health check (HEAD) |
| [**PostAction**](DefaultApi.md#postaction) | **POST** /actions/{action} | Execute a versioned knowledge-service action |
| [**PostCodeBuild**](DefaultApi.md#postcodebuild) | **POST** /code/build | Build a project knowledge index |
| [**PostCodeScan**](DefaultApi.md#postcodescan) | **POST** /code/scan | Request a canonical code index scan |
| [**PostCodeUpdate**](DefaultApi.md#postcodeupdate) | **POST** /code/update | Incrementally update a project knowledge index |
| [**PostDocs**](DefaultApi.md#postdocs) | **POST** /docs | Upload a document for ingest |
| [**PostDocsManifest**](DefaultApi.md#postdocsmanifest) | **POST** /docs/manifest | Check which uploaded document hashes are absent |
| [**PostDrain**](DefaultApi.md#postdrain) | **POST** /drain | Drain the asynchronous knowledge ingest queue |
| [**PostEntitySearch**](DefaultApi.md#postentitysearch) | **POST** /entities/search | Find entities by name or context |
| [**PostIngest**](DefaultApi.md#postingest) | **POST** /ingest | Enqueue background project ingest |
| [**PostIntelligenceBanditClose**](DefaultApi.md#postintelligencebanditclose) | **POST** /intelligence/bandit/close | Close a sampled decision with its observed reward |
| [**PostIntelligenceBanditPromote**](DefaultApi.md#postintelligencebanditpromote) | **POST** /intelligence/bandit/promote | Persist the production-default arm for a decision point |
| [**PostIntelligenceBanditSample**](DefaultApi.md#postintelligencebanditsample) | **POST** /intelligence/bandit/sample | Sample an arm for a decision point (server-side decision points) |
| [**PostMaintenanceClear**](DefaultApi.md#postmaintenanceclear) | **POST** /maintenance/clear | Clear indexed knowledge for a project |
| [**PostMaintenanceReconcile**](DefaultApi.md#postmaintenancereconcile) | **POST** /maintenance/reconcile | Reconcile orphaned vector records |
| [**PostMaintenanceRepair**](DefaultApi.md#postmaintenancerepair) | **POST** /maintenance/repair | Repair a project knowledge index |
| [**PostPromote**](DefaultApi.md#postpromote) | **POST** /releases/{id}/promote | Promote a release to active |
| [**PostReleases**](DefaultApi.md#postreleases) | **POST** /releases | Create a new corpus release |
| [**PostReviewAccept**](DefaultApi.md#postreviewaccept) | **POST** /review/{id}/accept | Accept a staged document |
| [**PostReviewReject**](DefaultApi.md#postreviewreject) | **POST** /review/{id}/reject | Reject a staged document |
| [**PostRollback**](DefaultApi.md#postrollback) | **POST** /releases/{id}/rollback | Roll back to a prior release |
| [**PostSearch**](DefaultApi.md#postsearch) | **POST** /search | Hybrid knowledge search |

<a id="deletedoc"></a>
# **DeleteDoc**
> void DeleteDoc (long id)

Delete a staged document


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **id** | **long** |  |  |

### Return type

void (empty response body)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: Not defined


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Document deleted |  -  |
| **401** | Unauthorized |  -  |
| **404** | Document not found |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="getactiverelease"></a>
# **GetActiveRelease**
> ActiveReleaseResponse GetActiveRelease ()

Get the currently active corpus release


### Parameters
This endpoint does not need any parameter.
### Return type

[**ActiveReleaseResponse**](ActiveReleaseResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Active release (or null if none) |  -  |
| **401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="getartifact"></a>
# **GetArtifact**
> ArtifactResponse GetArtifact (string id)

Retrieve an artifact by UUID


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **id** | **string** |  |  |

### Return type

[**ArtifactResponse**](ArtifactResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Artifact payload and citations |  -  |
| **401** | Unauthorized |  -  |
| **404** | Artifact not found |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="getartifactlinks"></a>
# **GetArtifactLinks**
> ArtifactLinksResponse GetArtifactLinks (string id)

Retrieve outgoing links from an artifact


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **id** | **string** |  |  |

### Return type

[**ArtifactLinksResponse**](ArtifactLinksResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Artifact links |  -  |
| **401** | Unauthorized |  -  |
| **404** | Artifact not found |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="getcapabilities"></a>
# **GetCapabilities**
> CapabilitiesResponse GetCapabilities ()

Advertised capabilities

Returns the set of capability strings this aimee-kb instance supports. Phase 1 always returns [\"memory\", \"search\", \"index\"]. 


### Parameters
This endpoint does not need any parameter.
### Return type

[**CapabilitiesResponse**](CapabilitiesResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Capability list |  -  |
| **401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="getcodeblastradius"></a>
# **GetCodeBlastRadius**
> BlastRadiusResponse GetCodeBlastRadius (string project, string filePath = null)

Blast-radius computation for a file


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **project** | **string** |  |  |
| **filePath** | **string** |  | [optional]  |

### Return type

[**BlastRadiusResponse**](BlastRadiusResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Blast radius |  -  |
| **400** | Missing required parameters |  -  |
| **401** | Unauthorized |  -  |
| **404** | File not found in index |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="getcodecallers"></a>
# **GetCodeCallers**
> CodeCallersResponse GetCodeCallers (string symbol, string project = null, int maxResults = null)

Call sites for a symbol in the canonical code index


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **symbol** | **string** |  |  |
| **project** | **string** |  | [optional]  |
| **maxResults** | **int** |  | [optional] [default to 20] |

### Return type

[**CodeCallersResponse**](CodeCallersResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Caller results |  -  |
| **400** | Missing symbol parameter |  -  |
| **401** | Unauthorized |  -  |
| **503** | Canonical index unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="getcodefind"></a>
# **GetCodeFind**
> CodeFindResponse GetCodeFind (string identifier, string project = null, int maxResults = null)

Symbol/identifier lookup across the canonical index


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **identifier** | **string** |  |  |
| **project** | **string** |  | [optional]  |
| **maxResults** | **int** |  | [optional] [default to 20] |

### Return type

[**CodeFindResponse**](CodeFindResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Code find results |  -  |
| **400** | Missing identifier parameter |  -  |
| **401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="getcodeprojectstats"></a>
# **GetCodeProjectStats**
> CodeProjectStatsResponse GetCodeProjectStats (string project)

Project-level canonical code index counts and language breakdown


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **project** | **string** |  |  |

### Return type

[**CodeProjectStatsResponse**](CodeProjectStatsResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Project index statistics |  -  |
| **400** | Missing required parameters |  -  |
| **401** | Unauthorized |  -  |
| **503** | Canonical index unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="getcodeprojects"></a>
# **GetCodeProjects**
> CodeProjectsResponse GetCodeProjects (int maxResults = null)

List projects in the canonical code index


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **maxResults** | **int** |  | [optional] [default to 100] |

### Return type

[**CodeProjectsResponse**](CodeProjectsResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Indexed projects |  -  |
| **401** | Unauthorized |  -  |
| **405** | Method not allowed |  -  |
| **503** | Canonical index unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="getcodesearch"></a>
# **GetCodeSearch**
> CodeSearchResponse GetCodeSearch (string query, string project = null, int maxResults = null)

Full-text code search across indexed file contents


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **query** | **string** |  |  |
| **project** | **string** |  | [optional]  |
| **maxResults** | **int** |  | [optional] [default to 20] |

### Return type

[**CodeSearchResponse**](CodeSearchResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Code search results |  -  |
| **400** | Missing query parameter |  -  |
| **401** | Unauthorized |  -  |
| **503** | Canonical index unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="getcodestructure"></a>
# **GetCodeStructure**
> CodeStructureResponse GetCodeStructure (string project, string filePath, int maxResults = null)

Definitions for a file in the canonical code index


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **project** | **string** |  |  |
| **filePath** | **string** |  |  |
| **maxResults** | **int** |  | [optional] [default to 256] |

### Return type

[**CodeStructureResponse**](CodeStructureResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | File definitions |  -  |
| **400** | Missing required parameters |  -  |
| **401** | Unauthorized |  -  |
| **503** | Canonical index unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="getdoc"></a>
# **GetDoc**
> DocMetadataResponse GetDoc (long id)

Retrieve doc metadata by id


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **id** | **long** |  |  |

### Return type

[**DocMetadataResponse**](DocMetadataResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Document metadata |  -  |
| **401** | Unauthorized |  -  |
| **404** | Document not found |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="getentityprofile"></a>
# **GetEntityProfile**
> EntityProfileResponse GetEntityProfile (string id)

Canonical entity profile


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **id** | **string** | Entity name (slug) |  |

### Return type

[**EntityProfileResponse**](EntityProfileResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Entity profile |  -  |
| **401** | Unauthorized |  -  |
| **404** | Entity not found |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="gethealth"></a>
# **GetHealth**
> HealthResponse GetHealth ()

Service health check

Returns {\"status\":\"ok\"} when the service is running.


### Parameters
This endpoint does not need any parameter.
### Return type

[**HealthResponse**](HealthResponse.md)

### Authorization

No authorization required

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Service is healthy |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="getingeststatus"></a>
# **GetIngestStatus**
> IngestStatusResponse GetIngestStatus ()

Report background project ingest status


### Parameters
This endpoint does not need any parameter.
### Return type

[**IngestStatusResponse**](IngestStatusResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Background ingest status |  -  |
| **401** | Unauthorized |  -  |
| **405** | Method not allowed |  -  |
| **503** | Ingest status unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="getintelligencebanditexport"></a>
# **GetIntelligenceBanditExport**
> Object GetIntelligenceBanditExport ()

Export fusion bandit decision data


### Parameters
This endpoint does not need any parameter.
### Return type

**Object**

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Bandit decisions and arm stats |  -  |
| **401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="getintelligencecalibrationreadiness"></a>
# **GetIntelligenceCalibrationReadiness**
> Object GetIntelligenceCalibrationReadiness ()

Calibration readiness


### Parameters
This endpoint does not need any parameter.
### Return type

**Object**

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Calibration readiness summary |  -  |
| **401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="getintelligencedemotioncheck"></a>
# **GetIntelligenceDemotionCheck**
> Object GetIntelligenceDemotionCheck ()

Dry-run demotion readiness check


### Parameters
This endpoint does not need any parameter.
### Return type

**Object**

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Demotion readiness summary |  -  |
| **401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="getjobstatus"></a>
# **GetJobStatus**
> JobStatusResponse GetJobStatus (long jobId)

Report asynchronous knowledge ingest job status


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **jobId** | **long** |  |  |

### Return type

[**JobStatusResponse**](JobStatusResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Job status |  -  |
| **401** | Unauthorized |  -  |
| **404** | Job not found |  -  |
| **405** | Method not allowed |  -  |
| **503** | Job status unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="getpipelinestatus"></a>
# **GetPipelineStatus**
> PipelineStatusResponse GetPipelineStatus ()

Report asynchronous knowledge ingest queue status


### Parameters
This endpoint does not need any parameter.
### Return type

[**PipelineStatusResponse**](PipelineStatusResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Pipeline status |  -  |
| **401** | Unauthorized |  -  |
| **405** | Method not allowed |  -  |
| **503** | Queue unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="getreview"></a>
# **GetReview**
> ReviewQueueResponse GetReview (long cursor = null, int limit = null)

List staged documents pending review


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **cursor** | **long** |  | [optional]  |
| **limit** | **int** |  | [optional] [default to 10] |

### Return type

[**ReviewQueueResponse**](ReviewQueueResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Review queue |  -  |
| **401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="getversion"></a>
# **GetVersion**
> VersionResponse GetVersion ()

Service version


### Parameters
This endpoint does not need any parameter.
### Return type

[**VersionResponse**](VersionResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Version information |  -  |
| **401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="getworkers"></a>
# **GetWorkers**
> WorkersResponse GetWorkers ()

Report aimee-kb worker and background task status


### Parameters
This endpoint does not need any parameter.
### Return type

[**WorkersResponse**](WorkersResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Worker status |  -  |
| **401** | Unauthorized |  -  |
| **405** | Method not allowed |  -  |
| **503** | Workers unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="headhealth"></a>
# **HeadHealth**
> void HeadHealth ()

Service health check (HEAD)


### Parameters
This endpoint does not need any parameter.
### Return type

void (empty response body)

### Authorization

No authorization required

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: Not defined


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Service is healthy |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="postaction"></a>
# **PostAction**
> Object PostAction (string action, Object body = null)

Execute a versioned knowledge-service action


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **action** | **string** |  |  |
| **body** | **Object** |  | [optional]  |

### Return type

**Object**

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Action response |  -  |
| **401** | Unauthorized |  -  |
| **404** | Unknown action |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="postcodebuild"></a>
# **PostCodeBuild**
> CodeBuildResponse PostCodeBuild (CodeBuildRequest codeBuildRequest)

Build a project knowledge index


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **codeBuildRequest** | [**CodeBuildRequest**](CodeBuildRequest.md) |  |  |

### Return type

[**CodeBuildResponse**](CodeBuildResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Build complete |  -  |
| **400** | Missing required parameters |  -  |
| **401** | Unauthorized |  -  |
| **503** | Knowledge or vector store unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="postcodescan"></a>
# **PostCodeScan**
> CodeScanResponse PostCodeScan (CodeScanRequest codeScanRequest)

Request a canonical code index scan


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **codeScanRequest** | [**CodeScanRequest**](CodeScanRequest.md) |  |  |

### Return type

[**CodeScanResponse**](CodeScanResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **202** | Scan accepted |  -  |
| **401** | Unauthorized |  -  |
| **405** | Method not allowed |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="postcodeupdate"></a>
# **PostCodeUpdate**
> CodeUpdateResponse PostCodeUpdate (CodeUpdateRequest codeUpdateRequest)

Incrementally update a project knowledge index


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **codeUpdateRequest** | [**CodeUpdateRequest**](CodeUpdateRequest.md) |  |  |

### Return type

[**CodeUpdateResponse**](CodeUpdateResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Update complete |  -  |
| **400** | Missing required parameters |  -  |
| **401** | Unauthorized |  -  |
| **503** | Knowledge store unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="postdocs"></a>
# **PostDocs**
> DocIngestResponse PostDocs (System.IO.Stream file, string scope = null)

Upload a document for ingest

Accepts multipart/form-data with a required `file` part and an optional `scope` field (default: global). Normalizes to markdown, stores in DB2, and queues an extract_doc job. Returns existing doc_id on idempotent re-upload. 


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **file** | **System.IO.Stream****System.IO.Stream** |  |  |
| **scope** | **string** |  | [optional] [default to &quot;global&quot;] |

### Return type

[**DocIngestResponse**](DocIngestResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: multipart/form-data
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **201** | Document ingested |  -  |
| **200** | Idempotent re-upload (existing doc_id returned) |  -  |
| **400** | Missing file part |  -  |
| **401** | Unauthorized |  -  |
| **422** | Normalization failed (converter error) |  -  |
| **503** | DB unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="postdocsmanifest"></a>
# **PostDocsManifest**
> DocsManifestResponse PostDocsManifest (DocsManifestRequest docsManifestRequest)

Check which uploaded document hashes are absent


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **docsManifestRequest** | [**DocsManifestRequest**](DocsManifestRequest.md) |  |  |

### Return type

[**DocsManifestResponse**](DocsManifestResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Manifest diff |  -  |
| **400** | Invalid manifest request |  -  |
| **401** | Unauthorized |  -  |
| **405** | Method not allowed |  -  |
| **500** | Serialization failed |  -  |
| **503** | DB unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="postdrain"></a>
# **PostDrain**
> DrainResponse PostDrain (DrainRequest drainRequest = null)

Drain the asynchronous knowledge ingest queue


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **drainRequest** | [**DrainRequest**](DrainRequest.md) |  | [optional]  |

### Return type

[**DrainResponse**](DrainResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Queue drained |  -  |
| **401** | Unauthorized |  -  |
| **405** | Method not allowed |  -  |
| **500** | Queue drain failed |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="postentitysearch"></a>
# **PostEntitySearch**
> EntitySearchResponse PostEntitySearch (EntitySearchRequest entitySearchRequest)

Find entities by name or context


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **entitySearchRequest** | [**EntitySearchRequest**](EntitySearchRequest.md) |  |  |

### Return type

[**EntitySearchResponse**](EntitySearchResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Entity search results |  -  |
| **400** | Missing query |  -  |
| **401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="postingest"></a>
# **PostIngest**
> IngestResponse PostIngest (IngestRequest ingestRequest = null)

Enqueue background project ingest


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **ingestRequest** | [**IngestRequest**](IngestRequest.md) |  | [optional]  |

### Return type

[**IngestResponse**](IngestResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **202** | Ingest queued |  -  |
| **401** | Unauthorized |  -  |
| **405** | Method not allowed |  -  |
| **503** | Knowledge store unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="postintelligencebanditclose"></a>
# **PostIntelligenceBanditClose**
> Object PostIntelligenceBanditClose ()

Close a sampled decision with its observed reward


### Parameters
This endpoint does not need any parameter.
### Return type

**Object**

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Close result |  -  |
| **401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="postintelligencebanditpromote"></a>
# **PostIntelligenceBanditPromote**
> Object PostIntelligenceBanditPromote ()

Persist the production-default arm for a decision point


### Parameters
This endpoint does not need any parameter.
### Return type

**Object**

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Promotion result (rollback_arm) |  -  |
| **401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="postintelligencebanditsample"></a>
# **PostIntelligenceBanditSample**
> Object PostIntelligenceBanditSample ()

Sample an arm for a decision point (server-side decision points)


### Parameters
This endpoint does not need any parameter.
### Return type

**Object**

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Selected arm + decision id (or status disabled) |  -  |
| **401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="postmaintenanceclear"></a>
# **PostMaintenanceClear**
> MaintenanceClearResponse PostMaintenanceClear (MaintenanceClearRequest maintenanceClearRequest)

Clear indexed knowledge for a project


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **maintenanceClearRequest** | [**MaintenanceClearRequest**](MaintenanceClearRequest.md) |  |  |

### Return type

[**MaintenanceClearResponse**](MaintenanceClearResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Project cleared |  -  |
| **400** | Missing project |  -  |
| **401** | Unauthorized |  -  |
| **500** | Clear failed |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="postmaintenancereconcile"></a>
# **PostMaintenanceReconcile**
> MaintenanceReconcileResponse PostMaintenanceReconcile (MaintenanceReconcileRequest maintenanceReconcileRequest = null)

Reconcile orphaned vector records


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **maintenanceReconcileRequest** | [**MaintenanceReconcileRequest**](MaintenanceReconcileRequest.md) |  | [optional]  |

### Return type

[**MaintenanceReconcileResponse**](MaintenanceReconcileResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Reconcile complete |  -  |
| **401** | Unauthorized |  -  |
| **500** | Reconcile failed |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="postmaintenancerepair"></a>
# **PostMaintenanceRepair**
> MaintenanceRepairResponse PostMaintenanceRepair (MaintenanceRepairRequest maintenanceRepairRequest)

Repair a project knowledge index


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **maintenanceRepairRequest** | [**MaintenanceRepairRequest**](MaintenanceRepairRequest.md) |  |  |

### Return type

[**MaintenanceRepairResponse**](MaintenanceRepairResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Repair complete |  -  |
| **400** | Missing required parameters |  -  |
| **401** | Unauthorized |  -  |
| **503** | Knowledge or vector store unavailable |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="postpromote"></a>
# **PostPromote**
> void PostPromote (long id)

Promote a release to active


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **id** | **long** |  |  |

### Return type

void (empty response body)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: Not defined


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Release promoted |  -  |
| **400** | Invalid id |  -  |
| **401** | Unauthorized |  -  |
| **409** | Promote failed |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="postreleases"></a>
# **PostReleases**
> CreateReleaseResponse PostReleases (CreateReleaseRequest createReleaseRequest)

Create a new corpus release


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **createReleaseRequest** | [**CreateReleaseRequest**](CreateReleaseRequest.md) |  |  |

### Return type

[**CreateReleaseResponse**](CreateReleaseResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **201** | Release created |  -  |
| **400** | Missing name |  -  |
| **401** | Unauthorized |  -  |
| **409** | Name already exists |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="postreviewaccept"></a>
# **PostReviewAccept**
> void PostReviewAccept (long id, ReviewAcceptRequest reviewAcceptRequest = null)

Accept a staged document


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **id** | **long** |  |  |
| **reviewAcceptRequest** | [**ReviewAcceptRequest**](ReviewAcceptRequest.md) |  | [optional]  |

### Return type

void (empty response body)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: Not defined


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Document accepted |  -  |
| **400** | Invalid id |  -  |
| **401** | Unauthorized |  -  |
| **404** | Document not found |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="postreviewreject"></a>
# **PostReviewReject**
> void PostReviewReject (long id, ReviewRejectRequest reviewRejectRequest)

Reject a staged document


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **id** | **long** |  |  |
| **reviewRejectRequest** | [**ReviewRejectRequest**](ReviewRejectRequest.md) |  |  |

### Return type

void (empty response body)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: Not defined


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Document rejected |  -  |
| **400** | Invalid id |  -  |
| **401** | Unauthorized |  -  |
| **404** | Document not found |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="postrollback"></a>
# **PostRollback**
> void PostRollback (long id, RollbackRequest rollbackRequest = null)

Roll back to a prior release


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **id** | **long** |  |  |
| **rollbackRequest** | [**RollbackRequest**](RollbackRequest.md) |  | [optional]  |

### Return type

void (empty response body)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: Not defined


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Rolled back |  -  |
| **400** | Invalid id |  -  |
| **401** | Unauthorized |  -  |
| **409** | No prior release to roll back to |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

<a id="postsearch"></a>
# **PostSearch**
> SearchResponse PostSearch (SearchRequest searchRequest)

Hybrid knowledge search


### Parameters

| Name | Type | Description | Notes |
|------|------|-------------|-------|
| **searchRequest** | [**SearchRequest**](SearchRequest.md) |  |  |

### Return type

[**SearchResponse**](SearchResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json


### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Search results |  -  |
| **400** | Bad request (missing query) |  -  |
| **401** | Unauthorized |  -  |

[[Back to top]](#) [[Back to API list]](../../README.md#documentation-for-api-endpoints) [[Back to Model list]](../../README.md#documentation-for-models) [[Back to README]](../../README.md)

