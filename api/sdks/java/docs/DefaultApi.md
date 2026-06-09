# DefaultApi

All URIs are relative to *http://127.0.0.1:8090/v1*

| Method | HTTP request | Description |
|------------- | ------------- | -------------|
| [**deleteDoc**](DefaultApi.md#deleteDoc) | **DELETE** /docs/{id} | Delete a staged document |
| [**deleteDocWithHttpInfo**](DefaultApi.md#deleteDocWithHttpInfo) | **DELETE** /docs/{id} | Delete a staged document |
| [**getActiveRelease**](DefaultApi.md#getActiveRelease) | **GET** /releases/active | Get the currently active corpus release |
| [**getActiveReleaseWithHttpInfo**](DefaultApi.md#getActiveReleaseWithHttpInfo) | **GET** /releases/active | Get the currently active corpus release |
| [**getArtifact**](DefaultApi.md#getArtifact) | **GET** /artifacts/{id} | Retrieve an artifact by UUID |
| [**getArtifactWithHttpInfo**](DefaultApi.md#getArtifactWithHttpInfo) | **GET** /artifacts/{id} | Retrieve an artifact by UUID |
| [**getArtifactLinks**](DefaultApi.md#getArtifactLinks) | **GET** /artifacts/{id}/links | Retrieve outgoing links from an artifact |
| [**getArtifactLinksWithHttpInfo**](DefaultApi.md#getArtifactLinksWithHttpInfo) | **GET** /artifacts/{id}/links | Retrieve outgoing links from an artifact |
| [**getCapabilities**](DefaultApi.md#getCapabilities) | **GET** /capabilities | Advertised capabilities |
| [**getCapabilitiesWithHttpInfo**](DefaultApi.md#getCapabilitiesWithHttpInfo) | **GET** /capabilities | Advertised capabilities |
| [**getCodeBlastRadius**](DefaultApi.md#getCodeBlastRadius) | **GET** /code/blast-radius | Blast-radius computation for a file |
| [**getCodeBlastRadiusWithHttpInfo**](DefaultApi.md#getCodeBlastRadiusWithHttpInfo) | **GET** /code/blast-radius | Blast-radius computation for a file |
| [**getCodeCallers**](DefaultApi.md#getCodeCallers) | **GET** /code/callers | Call sites for a symbol in the canonical code index |
| [**getCodeCallersWithHttpInfo**](DefaultApi.md#getCodeCallersWithHttpInfo) | **GET** /code/callers | Call sites for a symbol in the canonical code index |
| [**getCodeFind**](DefaultApi.md#getCodeFind) | **GET** /code/find | Symbol/identifier lookup across the canonical index |
| [**getCodeFindWithHttpInfo**](DefaultApi.md#getCodeFindWithHttpInfo) | **GET** /code/find | Symbol/identifier lookup across the canonical index |
| [**getCodeProjectStats**](DefaultApi.md#getCodeProjectStats) | **GET** /code/project-stats | Project-level canonical code index counts and language breakdown |
| [**getCodeProjectStatsWithHttpInfo**](DefaultApi.md#getCodeProjectStatsWithHttpInfo) | **GET** /code/project-stats | Project-level canonical code index counts and language breakdown |
| [**getCodeProjects**](DefaultApi.md#getCodeProjects) | **GET** /code/projects | List projects in the canonical code index |
| [**getCodeProjectsWithHttpInfo**](DefaultApi.md#getCodeProjectsWithHttpInfo) | **GET** /code/projects | List projects in the canonical code index |
| [**getCodeSearch**](DefaultApi.md#getCodeSearch) | **GET** /code/search | Full-text code search across indexed file contents |
| [**getCodeSearchWithHttpInfo**](DefaultApi.md#getCodeSearchWithHttpInfo) | **GET** /code/search | Full-text code search across indexed file contents |
| [**getCodeStructure**](DefaultApi.md#getCodeStructure) | **GET** /code/structure | Definitions for a file in the canonical code index |
| [**getCodeStructureWithHttpInfo**](DefaultApi.md#getCodeStructureWithHttpInfo) | **GET** /code/structure | Definitions for a file in the canonical code index |
| [**getDoc**](DefaultApi.md#getDoc) | **GET** /docs/{id} | Retrieve doc metadata by id |
| [**getDocWithHttpInfo**](DefaultApi.md#getDocWithHttpInfo) | **GET** /docs/{id} | Retrieve doc metadata by id |
| [**getEntityProfile**](DefaultApi.md#getEntityProfile) | **GET** /entities/{id} | Canonical entity profile |
| [**getEntityProfileWithHttpInfo**](DefaultApi.md#getEntityProfileWithHttpInfo) | **GET** /entities/{id} | Canonical entity profile |
| [**getHealth**](DefaultApi.md#getHealth) | **GET** /health | Service health check |
| [**getHealthWithHttpInfo**](DefaultApi.md#getHealthWithHttpInfo) | **GET** /health | Service health check |
| [**getIngestStatus**](DefaultApi.md#getIngestStatus) | **GET** /ingest/status | Report background project ingest status |
| [**getIngestStatusWithHttpInfo**](DefaultApi.md#getIngestStatusWithHttpInfo) | **GET** /ingest/status | Report background project ingest status |
| [**getIntelligenceBanditExport**](DefaultApi.md#getIntelligenceBanditExport) | **GET** /intelligence/bandit/export | Export fusion bandit decision data |
| [**getIntelligenceBanditExportWithHttpInfo**](DefaultApi.md#getIntelligenceBanditExportWithHttpInfo) | **GET** /intelligence/bandit/export | Export fusion bandit decision data |
| [**getIntelligenceCalibrationReadiness**](DefaultApi.md#getIntelligenceCalibrationReadiness) | **GET** /intelligence/calibration/readiness | Calibration readiness |
| [**getIntelligenceCalibrationReadinessWithHttpInfo**](DefaultApi.md#getIntelligenceCalibrationReadinessWithHttpInfo) | **GET** /intelligence/calibration/readiness | Calibration readiness |
| [**getIntelligenceDemotionCheck**](DefaultApi.md#getIntelligenceDemotionCheck) | **GET** /intelligence/demotion/check | Dry-run demotion readiness check |
| [**getIntelligenceDemotionCheckWithHttpInfo**](DefaultApi.md#getIntelligenceDemotionCheckWithHttpInfo) | **GET** /intelligence/demotion/check | Dry-run demotion readiness check |
| [**getJobStatus**](DefaultApi.md#getJobStatus) | **GET** /jobs/{job_id} | Report asynchronous knowledge ingest job status |
| [**getJobStatusWithHttpInfo**](DefaultApi.md#getJobStatusWithHttpInfo) | **GET** /jobs/{job_id} | Report asynchronous knowledge ingest job status |
| [**getPipelineStatus**](DefaultApi.md#getPipelineStatus) | **GET** /pipeline/status | Report asynchronous knowledge ingest queue status |
| [**getPipelineStatusWithHttpInfo**](DefaultApi.md#getPipelineStatusWithHttpInfo) | **GET** /pipeline/status | Report asynchronous knowledge ingest queue status |
| [**getReview**](DefaultApi.md#getReview) | **GET** /review | List staged documents pending review |
| [**getReviewWithHttpInfo**](DefaultApi.md#getReviewWithHttpInfo) | **GET** /review | List staged documents pending review |
| [**getVersion**](DefaultApi.md#getVersion) | **GET** /version | Service version |
| [**getVersionWithHttpInfo**](DefaultApi.md#getVersionWithHttpInfo) | **GET** /version | Service version |
| [**getWorkers**](DefaultApi.md#getWorkers) | **GET** /workers | Report aimee-kb worker and background task status |
| [**getWorkersWithHttpInfo**](DefaultApi.md#getWorkersWithHttpInfo) | **GET** /workers | Report aimee-kb worker and background task status |
| [**headHealth**](DefaultApi.md#headHealth) | **HEAD** /health | Service health check (HEAD) |
| [**headHealthWithHttpInfo**](DefaultApi.md#headHealthWithHttpInfo) | **HEAD** /health | Service health check (HEAD) |
| [**postAction**](DefaultApi.md#postAction) | **POST** /actions/{action} | Execute a versioned knowledge-service action |
| [**postActionWithHttpInfo**](DefaultApi.md#postActionWithHttpInfo) | **POST** /actions/{action} | Execute a versioned knowledge-service action |
| [**postCodeBuild**](DefaultApi.md#postCodeBuild) | **POST** /code/build | Build a project knowledge index |
| [**postCodeBuildWithHttpInfo**](DefaultApi.md#postCodeBuildWithHttpInfo) | **POST** /code/build | Build a project knowledge index |
| [**postCodeScan**](DefaultApi.md#postCodeScan) | **POST** /code/scan | Request a canonical code index scan |
| [**postCodeScanWithHttpInfo**](DefaultApi.md#postCodeScanWithHttpInfo) | **POST** /code/scan | Request a canonical code index scan |
| [**postCodeUpdate**](DefaultApi.md#postCodeUpdate) | **POST** /code/update | Incrementally update a project knowledge index |
| [**postCodeUpdateWithHttpInfo**](DefaultApi.md#postCodeUpdateWithHttpInfo) | **POST** /code/update | Incrementally update a project knowledge index |
| [**postDocs**](DefaultApi.md#postDocs) | **POST** /docs | Upload a document for ingest |
| [**postDocsWithHttpInfo**](DefaultApi.md#postDocsWithHttpInfo) | **POST** /docs | Upload a document for ingest |
| [**postDocsManifest**](DefaultApi.md#postDocsManifest) | **POST** /docs/manifest | Check which uploaded document hashes are absent |
| [**postDocsManifestWithHttpInfo**](DefaultApi.md#postDocsManifestWithHttpInfo) | **POST** /docs/manifest | Check which uploaded document hashes are absent |
| [**postDrain**](DefaultApi.md#postDrain) | **POST** /drain | Drain the asynchronous knowledge ingest queue |
| [**postDrainWithHttpInfo**](DefaultApi.md#postDrainWithHttpInfo) | **POST** /drain | Drain the asynchronous knowledge ingest queue |
| [**postEntitySearch**](DefaultApi.md#postEntitySearch) | **POST** /entities/search | Find entities by name or context |
| [**postEntitySearchWithHttpInfo**](DefaultApi.md#postEntitySearchWithHttpInfo) | **POST** /entities/search | Find entities by name or context |
| [**postIngest**](DefaultApi.md#postIngest) | **POST** /ingest | Enqueue background project ingest |
| [**postIngestWithHttpInfo**](DefaultApi.md#postIngestWithHttpInfo) | **POST** /ingest | Enqueue background project ingest |
| [**postIntelligenceBanditClose**](DefaultApi.md#postIntelligenceBanditClose) | **POST** /intelligence/bandit/close | Close a sampled decision with its observed reward |
| [**postIntelligenceBanditCloseWithHttpInfo**](DefaultApi.md#postIntelligenceBanditCloseWithHttpInfo) | **POST** /intelligence/bandit/close | Close a sampled decision with its observed reward |
| [**postIntelligenceBanditPromote**](DefaultApi.md#postIntelligenceBanditPromote) | **POST** /intelligence/bandit/promote | Persist the production-default arm for a decision point |
| [**postIntelligenceBanditPromoteWithHttpInfo**](DefaultApi.md#postIntelligenceBanditPromoteWithHttpInfo) | **POST** /intelligence/bandit/promote | Persist the production-default arm for a decision point |
| [**postIntelligenceBanditSample**](DefaultApi.md#postIntelligenceBanditSample) | **POST** /intelligence/bandit/sample | Sample an arm for a decision point (server-side decision points) |
| [**postIntelligenceBanditSampleWithHttpInfo**](DefaultApi.md#postIntelligenceBanditSampleWithHttpInfo) | **POST** /intelligence/bandit/sample | Sample an arm for a decision point (server-side decision points) |
| [**postMaintenanceClear**](DefaultApi.md#postMaintenanceClear) | **POST** /maintenance/clear | Clear indexed knowledge for a project |
| [**postMaintenanceClearWithHttpInfo**](DefaultApi.md#postMaintenanceClearWithHttpInfo) | **POST** /maintenance/clear | Clear indexed knowledge for a project |
| [**postMaintenanceReconcile**](DefaultApi.md#postMaintenanceReconcile) | **POST** /maintenance/reconcile | Reconcile orphaned vector records |
| [**postMaintenanceReconcileWithHttpInfo**](DefaultApi.md#postMaintenanceReconcileWithHttpInfo) | **POST** /maintenance/reconcile | Reconcile orphaned vector records |
| [**postMaintenanceRepair**](DefaultApi.md#postMaintenanceRepair) | **POST** /maintenance/repair | Repair a project knowledge index |
| [**postMaintenanceRepairWithHttpInfo**](DefaultApi.md#postMaintenanceRepairWithHttpInfo) | **POST** /maintenance/repair | Repair a project knowledge index |
| [**postPromote**](DefaultApi.md#postPromote) | **POST** /releases/{id}/promote | Promote a release to active |
| [**postPromoteWithHttpInfo**](DefaultApi.md#postPromoteWithHttpInfo) | **POST** /releases/{id}/promote | Promote a release to active |
| [**postReleases**](DefaultApi.md#postReleases) | **POST** /releases | Create a new corpus release |
| [**postReleasesWithHttpInfo**](DefaultApi.md#postReleasesWithHttpInfo) | **POST** /releases | Create a new corpus release |
| [**postReviewAccept**](DefaultApi.md#postReviewAccept) | **POST** /review/{id}/accept | Accept a staged document |
| [**postReviewAcceptWithHttpInfo**](DefaultApi.md#postReviewAcceptWithHttpInfo) | **POST** /review/{id}/accept | Accept a staged document |
| [**postReviewReject**](DefaultApi.md#postReviewReject) | **POST** /review/{id}/reject | Reject a staged document |
| [**postReviewRejectWithHttpInfo**](DefaultApi.md#postReviewRejectWithHttpInfo) | **POST** /review/{id}/reject | Reject a staged document |
| [**postRollback**](DefaultApi.md#postRollback) | **POST** /releases/{id}/rollback | Roll back to a prior release |
| [**postRollbackWithHttpInfo**](DefaultApi.md#postRollbackWithHttpInfo) | **POST** /releases/{id}/rollback | Roll back to a prior release |
| [**postSearch**](DefaultApi.md#postSearch) | **POST** /search | Hybrid knowledge search |
| [**postSearchWithHttpInfo**](DefaultApi.md#postSearchWithHttpInfo) | **POST** /search | Hybrid knowledge search |



## deleteDoc

> void deleteDoc(id)

Delete a staged document

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        Long id = 56L; // Long | 
        try {
            apiInstance.deleteDoc(id);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#deleteDoc");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **id** | **Long**|  | |

### Return type


null (empty response body)

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

## deleteDocWithHttpInfo

> ApiResponse<Void> deleteDocWithHttpInfo(id)

Delete a staged document

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        Long id = 56L; // Long | 
        try {
            ApiResponse<Void> response = apiInstance.deleteDocWithHttpInfo(id);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#deleteDoc");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **id** | **Long**|  | |

### Return type


ApiResponse<Void>

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


## getActiveRelease

> ActiveReleaseResponse getActiveRelease()

Get the currently active corpus release

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        try {
            ActiveReleaseResponse result = apiInstance.getActiveRelease();
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getActiveRelease");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

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

## getActiveReleaseWithHttpInfo

> ApiResponse<ActiveReleaseResponse> getActiveReleaseWithHttpInfo()

Get the currently active corpus release

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        try {
            ApiResponse<ActiveReleaseResponse> response = apiInstance.getActiveReleaseWithHttpInfo();
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getActiveRelease");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters

This endpoint does not need any parameter.

### Return type

ApiResponse<[**ActiveReleaseResponse**](ActiveReleaseResponse.md)>


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


## getArtifact

> ArtifactResponse getArtifact(id)

Retrieve an artifact by UUID

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        String id = "id_example"; // String | 
        try {
            ArtifactResponse result = apiInstance.getArtifact(id);
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getArtifact");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **id** | **String**|  | |

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

## getArtifactWithHttpInfo

> ApiResponse<ArtifactResponse> getArtifactWithHttpInfo(id)

Retrieve an artifact by UUID

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        String id = "id_example"; // String | 
        try {
            ApiResponse<ArtifactResponse> response = apiInstance.getArtifactWithHttpInfo(id);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getArtifact");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **id** | **String**|  | |

### Return type

ApiResponse<[**ArtifactResponse**](ArtifactResponse.md)>


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


## getArtifactLinks

> ArtifactLinksResponse getArtifactLinks(id)

Retrieve outgoing links from an artifact

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        String id = "id_example"; // String | 
        try {
            ArtifactLinksResponse result = apiInstance.getArtifactLinks(id);
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getArtifactLinks");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **id** | **String**|  | |

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

## getArtifactLinksWithHttpInfo

> ApiResponse<ArtifactLinksResponse> getArtifactLinksWithHttpInfo(id)

Retrieve outgoing links from an artifact

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        String id = "id_example"; // String | 
        try {
            ApiResponse<ArtifactLinksResponse> response = apiInstance.getArtifactLinksWithHttpInfo(id);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getArtifactLinks");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **id** | **String**|  | |

### Return type

ApiResponse<[**ArtifactLinksResponse**](ArtifactLinksResponse.md)>


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


## getCapabilities

> CapabilitiesResponse getCapabilities()

Advertised capabilities

Returns the set of capability strings this aimee-kb instance supports. Phase 1 always returns [\&quot;memory\&quot;, \&quot;search\&quot;, \&quot;index\&quot;]. 

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        try {
            CapabilitiesResponse result = apiInstance.getCapabilities();
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getCapabilities");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

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

## getCapabilitiesWithHttpInfo

> ApiResponse<CapabilitiesResponse> getCapabilitiesWithHttpInfo()

Advertised capabilities

Returns the set of capability strings this aimee-kb instance supports. Phase 1 always returns [\&quot;memory\&quot;, \&quot;search\&quot;, \&quot;index\&quot;]. 

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        try {
            ApiResponse<CapabilitiesResponse> response = apiInstance.getCapabilitiesWithHttpInfo();
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getCapabilities");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters

This endpoint does not need any parameter.

### Return type

ApiResponse<[**CapabilitiesResponse**](CapabilitiesResponse.md)>


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


## getCodeBlastRadius

> BlastRadiusResponse getCodeBlastRadius(project, filePath)

Blast-radius computation for a file

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        String project = "project_example"; // String | 
        String filePath = "filePath_example"; // String | 
        try {
            BlastRadiusResponse result = apiInstance.getCodeBlastRadius(project, filePath);
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getCodeBlastRadius");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **project** | **String**|  | |
| **filePath** | **String**|  | [optional] |

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

## getCodeBlastRadiusWithHttpInfo

> ApiResponse<BlastRadiusResponse> getCodeBlastRadiusWithHttpInfo(project, filePath)

Blast-radius computation for a file

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        String project = "project_example"; // String | 
        String filePath = "filePath_example"; // String | 
        try {
            ApiResponse<BlastRadiusResponse> response = apiInstance.getCodeBlastRadiusWithHttpInfo(project, filePath);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getCodeBlastRadius");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **project** | **String**|  | |
| **filePath** | **String**|  | [optional] |

### Return type

ApiResponse<[**BlastRadiusResponse**](BlastRadiusResponse.md)>


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


## getCodeCallers

> CodeCallersResponse getCodeCallers(symbol, project, maxResults)

Call sites for a symbol in the canonical code index

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        String symbol = "symbol_example"; // String | 
        String project = "project_example"; // String | 
        Integer maxResults = 20; // Integer | 
        try {
            CodeCallersResponse result = apiInstance.getCodeCallers(symbol, project, maxResults);
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getCodeCallers");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **symbol** | **String**|  | |
| **project** | **String**|  | [optional] |
| **maxResults** | **Integer**|  | [optional] [default to 20] |

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

## getCodeCallersWithHttpInfo

> ApiResponse<CodeCallersResponse> getCodeCallersWithHttpInfo(symbol, project, maxResults)

Call sites for a symbol in the canonical code index

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        String symbol = "symbol_example"; // String | 
        String project = "project_example"; // String | 
        Integer maxResults = 20; // Integer | 
        try {
            ApiResponse<CodeCallersResponse> response = apiInstance.getCodeCallersWithHttpInfo(symbol, project, maxResults);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getCodeCallers");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **symbol** | **String**|  | |
| **project** | **String**|  | [optional] |
| **maxResults** | **Integer**|  | [optional] [default to 20] |

### Return type

ApiResponse<[**CodeCallersResponse**](CodeCallersResponse.md)>


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


## getCodeFind

> CodeFindResponse getCodeFind(identifier, project, maxResults)

Symbol/identifier lookup across the canonical index

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        String identifier = "identifier_example"; // String | 
        String project = "project_example"; // String | 
        Integer maxResults = 20; // Integer | 
        try {
            CodeFindResponse result = apiInstance.getCodeFind(identifier, project, maxResults);
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getCodeFind");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **identifier** | **String**|  | |
| **project** | **String**|  | [optional] |
| **maxResults** | **Integer**|  | [optional] [default to 20] |

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

## getCodeFindWithHttpInfo

> ApiResponse<CodeFindResponse> getCodeFindWithHttpInfo(identifier, project, maxResults)

Symbol/identifier lookup across the canonical index

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        String identifier = "identifier_example"; // String | 
        String project = "project_example"; // String | 
        Integer maxResults = 20; // Integer | 
        try {
            ApiResponse<CodeFindResponse> response = apiInstance.getCodeFindWithHttpInfo(identifier, project, maxResults);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getCodeFind");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **identifier** | **String**|  | |
| **project** | **String**|  | [optional] |
| **maxResults** | **Integer**|  | [optional] [default to 20] |

### Return type

ApiResponse<[**CodeFindResponse**](CodeFindResponse.md)>


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


## getCodeProjectStats

> CodeProjectStatsResponse getCodeProjectStats(project)

Project-level canonical code index counts and language breakdown

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        String project = "project_example"; // String | 
        try {
            CodeProjectStatsResponse result = apiInstance.getCodeProjectStats(project);
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getCodeProjectStats");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **project** | **String**|  | |

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

## getCodeProjectStatsWithHttpInfo

> ApiResponse<CodeProjectStatsResponse> getCodeProjectStatsWithHttpInfo(project)

Project-level canonical code index counts and language breakdown

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        String project = "project_example"; // String | 
        try {
            ApiResponse<CodeProjectStatsResponse> response = apiInstance.getCodeProjectStatsWithHttpInfo(project);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getCodeProjectStats");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **project** | **String**|  | |

### Return type

ApiResponse<[**CodeProjectStatsResponse**](CodeProjectStatsResponse.md)>


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


## getCodeProjects

> CodeProjectsResponse getCodeProjects(maxResults)

List projects in the canonical code index

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        Integer maxResults = 100; // Integer | 
        try {
            CodeProjectsResponse result = apiInstance.getCodeProjects(maxResults);
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getCodeProjects");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **maxResults** | **Integer**|  | [optional] [default to 100] |

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

## getCodeProjectsWithHttpInfo

> ApiResponse<CodeProjectsResponse> getCodeProjectsWithHttpInfo(maxResults)

List projects in the canonical code index

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        Integer maxResults = 100; // Integer | 
        try {
            ApiResponse<CodeProjectsResponse> response = apiInstance.getCodeProjectsWithHttpInfo(maxResults);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getCodeProjects");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **maxResults** | **Integer**|  | [optional] [default to 100] |

### Return type

ApiResponse<[**CodeProjectsResponse**](CodeProjectsResponse.md)>


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


## getCodeSearch

> CodeSearchResponse getCodeSearch(query, project, maxResults)

Full-text code search across indexed file contents

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        String query = "query_example"; // String | 
        String project = "project_example"; // String | 
        Integer maxResults = 20; // Integer | 
        try {
            CodeSearchResponse result = apiInstance.getCodeSearch(query, project, maxResults);
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getCodeSearch");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **query** | **String**|  | |
| **project** | **String**|  | [optional] |
| **maxResults** | **Integer**|  | [optional] [default to 20] |

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

## getCodeSearchWithHttpInfo

> ApiResponse<CodeSearchResponse> getCodeSearchWithHttpInfo(query, project, maxResults)

Full-text code search across indexed file contents

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        String query = "query_example"; // String | 
        String project = "project_example"; // String | 
        Integer maxResults = 20; // Integer | 
        try {
            ApiResponse<CodeSearchResponse> response = apiInstance.getCodeSearchWithHttpInfo(query, project, maxResults);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getCodeSearch");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **query** | **String**|  | |
| **project** | **String**|  | [optional] |
| **maxResults** | **Integer**|  | [optional] [default to 20] |

### Return type

ApiResponse<[**CodeSearchResponse**](CodeSearchResponse.md)>


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


## getCodeStructure

> CodeStructureResponse getCodeStructure(project, filePath, maxResults)

Definitions for a file in the canonical code index

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        String project = "project_example"; // String | 
        String filePath = "filePath_example"; // String | 
        Integer maxResults = 256; // Integer | 
        try {
            CodeStructureResponse result = apiInstance.getCodeStructure(project, filePath, maxResults);
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getCodeStructure");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **project** | **String**|  | |
| **filePath** | **String**|  | |
| **maxResults** | **Integer**|  | [optional] [default to 256] |

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

## getCodeStructureWithHttpInfo

> ApiResponse<CodeStructureResponse> getCodeStructureWithHttpInfo(project, filePath, maxResults)

Definitions for a file in the canonical code index

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        String project = "project_example"; // String | 
        String filePath = "filePath_example"; // String | 
        Integer maxResults = 256; // Integer | 
        try {
            ApiResponse<CodeStructureResponse> response = apiInstance.getCodeStructureWithHttpInfo(project, filePath, maxResults);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getCodeStructure");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **project** | **String**|  | |
| **filePath** | **String**|  | |
| **maxResults** | **Integer**|  | [optional] [default to 256] |

### Return type

ApiResponse<[**CodeStructureResponse**](CodeStructureResponse.md)>


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


## getDoc

> DocMetadataResponse getDoc(id)

Retrieve doc metadata by id

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        Long id = 56L; // Long | 
        try {
            DocMetadataResponse result = apiInstance.getDoc(id);
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getDoc");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **id** | **Long**|  | |

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

## getDocWithHttpInfo

> ApiResponse<DocMetadataResponse> getDocWithHttpInfo(id)

Retrieve doc metadata by id

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        Long id = 56L; // Long | 
        try {
            ApiResponse<DocMetadataResponse> response = apiInstance.getDocWithHttpInfo(id);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getDoc");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **id** | **Long**|  | |

### Return type

ApiResponse<[**DocMetadataResponse**](DocMetadataResponse.md)>


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


## getEntityProfile

> EntityProfileResponse getEntityProfile(id)

Canonical entity profile

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        String id = "id_example"; // String | Entity name (slug)
        try {
            EntityProfileResponse result = apiInstance.getEntityProfile(id);
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getEntityProfile");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **id** | **String**| Entity name (slug) | |

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

## getEntityProfileWithHttpInfo

> ApiResponse<EntityProfileResponse> getEntityProfileWithHttpInfo(id)

Canonical entity profile

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        String id = "id_example"; // String | Entity name (slug)
        try {
            ApiResponse<EntityProfileResponse> response = apiInstance.getEntityProfileWithHttpInfo(id);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getEntityProfile");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **id** | **String**| Entity name (slug) | |

### Return type

ApiResponse<[**EntityProfileResponse**](EntityProfileResponse.md)>


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


## getHealth

> HealthResponse getHealth()

Service health check

Returns {\&quot;status\&quot;:\&quot;ok\&quot;} when the service is running.

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        try {
            HealthResponse result = apiInstance.getHealth();
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getHealth");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

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

## getHealthWithHttpInfo

> ApiResponse<HealthResponse> getHealthWithHttpInfo()

Service health check

Returns {\&quot;status\&quot;:\&quot;ok\&quot;} when the service is running.

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        try {
            ApiResponse<HealthResponse> response = apiInstance.getHealthWithHttpInfo();
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getHealth");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters

This endpoint does not need any parameter.

### Return type

ApiResponse<[**HealthResponse**](HealthResponse.md)>


### Authorization

No authorization required

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Service is healthy |  -  |


## getIngestStatus

> IngestStatusResponse getIngestStatus()

Report background project ingest status

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        try {
            IngestStatusResponse result = apiInstance.getIngestStatus();
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getIngestStatus");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

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

## getIngestStatusWithHttpInfo

> ApiResponse<IngestStatusResponse> getIngestStatusWithHttpInfo()

Report background project ingest status

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        try {
            ApiResponse<IngestStatusResponse> response = apiInstance.getIngestStatusWithHttpInfo();
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getIngestStatus");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters

This endpoint does not need any parameter.

### Return type

ApiResponse<[**IngestStatusResponse**](IngestStatusResponse.md)>


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


## getIntelligenceBanditExport

> Object getIntelligenceBanditExport()

Export fusion bandit decision data

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        try {
            Object result = apiInstance.getIntelligenceBanditExport();
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getIntelligenceBanditExport");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

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

## getIntelligenceBanditExportWithHttpInfo

> ApiResponse<Object> getIntelligenceBanditExportWithHttpInfo()

Export fusion bandit decision data

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        try {
            ApiResponse<Object> response = apiInstance.getIntelligenceBanditExportWithHttpInfo();
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getIntelligenceBanditExport");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters

This endpoint does not need any parameter.

### Return type

ApiResponse<**Object**>


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


## getIntelligenceCalibrationReadiness

> Object getIntelligenceCalibrationReadiness()

Calibration readiness

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        try {
            Object result = apiInstance.getIntelligenceCalibrationReadiness();
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getIntelligenceCalibrationReadiness");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

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

## getIntelligenceCalibrationReadinessWithHttpInfo

> ApiResponse<Object> getIntelligenceCalibrationReadinessWithHttpInfo()

Calibration readiness

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        try {
            ApiResponse<Object> response = apiInstance.getIntelligenceCalibrationReadinessWithHttpInfo();
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getIntelligenceCalibrationReadiness");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters

This endpoint does not need any parameter.

### Return type

ApiResponse<**Object**>


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


## getIntelligenceDemotionCheck

> Object getIntelligenceDemotionCheck()

Dry-run demotion readiness check

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        try {
            Object result = apiInstance.getIntelligenceDemotionCheck();
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getIntelligenceDemotionCheck");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

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

## getIntelligenceDemotionCheckWithHttpInfo

> ApiResponse<Object> getIntelligenceDemotionCheckWithHttpInfo()

Dry-run demotion readiness check

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        try {
            ApiResponse<Object> response = apiInstance.getIntelligenceDemotionCheckWithHttpInfo();
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getIntelligenceDemotionCheck");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters

This endpoint does not need any parameter.

### Return type

ApiResponse<**Object**>


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


## getJobStatus

> JobStatusResponse getJobStatus(jobId)

Report asynchronous knowledge ingest job status

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        Long jobId = 56L; // Long | 
        try {
            JobStatusResponse result = apiInstance.getJobStatus(jobId);
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getJobStatus");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **jobId** | **Long**|  | |

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

## getJobStatusWithHttpInfo

> ApiResponse<JobStatusResponse> getJobStatusWithHttpInfo(jobId)

Report asynchronous knowledge ingest job status

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        Long jobId = 56L; // Long | 
        try {
            ApiResponse<JobStatusResponse> response = apiInstance.getJobStatusWithHttpInfo(jobId);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getJobStatus");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **jobId** | **Long**|  | |

### Return type

ApiResponse<[**JobStatusResponse**](JobStatusResponse.md)>


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


## getPipelineStatus

> PipelineStatusResponse getPipelineStatus()

Report asynchronous knowledge ingest queue status

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        try {
            PipelineStatusResponse result = apiInstance.getPipelineStatus();
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getPipelineStatus");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

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

## getPipelineStatusWithHttpInfo

> ApiResponse<PipelineStatusResponse> getPipelineStatusWithHttpInfo()

Report asynchronous knowledge ingest queue status

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        try {
            ApiResponse<PipelineStatusResponse> response = apiInstance.getPipelineStatusWithHttpInfo();
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getPipelineStatus");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters

This endpoint does not need any parameter.

### Return type

ApiResponse<[**PipelineStatusResponse**](PipelineStatusResponse.md)>


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


## getReview

> ReviewQueueResponse getReview(cursor, limit)

List staged documents pending review

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        Long cursor = 56L; // Long | 
        Integer limit = 10; // Integer | 
        try {
            ReviewQueueResponse result = apiInstance.getReview(cursor, limit);
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getReview");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **cursor** | **Long**|  | [optional] |
| **limit** | **Integer**|  | [optional] [default to 10] |

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

## getReviewWithHttpInfo

> ApiResponse<ReviewQueueResponse> getReviewWithHttpInfo(cursor, limit)

List staged documents pending review

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        Long cursor = 56L; // Long | 
        Integer limit = 10; // Integer | 
        try {
            ApiResponse<ReviewQueueResponse> response = apiInstance.getReviewWithHttpInfo(cursor, limit);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getReview");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **cursor** | **Long**|  | [optional] |
| **limit** | **Integer**|  | [optional] [default to 10] |

### Return type

ApiResponse<[**ReviewQueueResponse**](ReviewQueueResponse.md)>


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


## getVersion

> VersionResponse getVersion()

Service version

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        try {
            VersionResponse result = apiInstance.getVersion();
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getVersion");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

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

## getVersionWithHttpInfo

> ApiResponse<VersionResponse> getVersionWithHttpInfo()

Service version

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        try {
            ApiResponse<VersionResponse> response = apiInstance.getVersionWithHttpInfo();
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getVersion");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters

This endpoint does not need any parameter.

### Return type

ApiResponse<[**VersionResponse**](VersionResponse.md)>


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


## getWorkers

> WorkersResponse getWorkers()

Report aimee-kb worker and background task status

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        try {
            WorkersResponse result = apiInstance.getWorkers();
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getWorkers");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

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

## getWorkersWithHttpInfo

> ApiResponse<WorkersResponse> getWorkersWithHttpInfo()

Report aimee-kb worker and background task status

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        try {
            ApiResponse<WorkersResponse> response = apiInstance.getWorkersWithHttpInfo();
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#getWorkers");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters

This endpoint does not need any parameter.

### Return type

ApiResponse<[**WorkersResponse**](WorkersResponse.md)>


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


## headHealth

> void headHealth()

Service health check (HEAD)

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        try {
            apiInstance.headHealth();
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#headHealth");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters

This endpoint does not need any parameter.

### Return type


null (empty response body)

### Authorization

No authorization required

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: Not defined

### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Service is healthy |  -  |

## headHealthWithHttpInfo

> ApiResponse<Void> headHealthWithHttpInfo()

Service health check (HEAD)

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        try {
            ApiResponse<Void> response = apiInstance.headHealthWithHttpInfo();
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#headHealth");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters

This endpoint does not need any parameter.

### Return type


ApiResponse<Void>

### Authorization

No authorization required

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: Not defined

### HTTP response details
| Status code | Description | Response headers |
|-------------|-------------|------------------|
| **200** | Service is healthy |  -  |


## postAction

> Object postAction(action, body)

Execute a versioned knowledge-service action

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        String action = "action_example"; // String | 
        Object body = null; // Object | 
        try {
            Object result = apiInstance.postAction(action, body);
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postAction");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **action** | **String**|  | |
| **body** | **Object**|  | [optional] |

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

## postActionWithHttpInfo

> ApiResponse<Object> postActionWithHttpInfo(action, body)

Execute a versioned knowledge-service action

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        String action = "action_example"; // String | 
        Object body = null; // Object | 
        try {
            ApiResponse<Object> response = apiInstance.postActionWithHttpInfo(action, body);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postAction");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **action** | **String**|  | |
| **body** | **Object**|  | [optional] |

### Return type

ApiResponse<**Object**>


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


## postCodeBuild

> CodeBuildResponse postCodeBuild(codeBuildRequest)

Build a project knowledge index

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        CodeBuildRequest codeBuildRequest = new CodeBuildRequest(); // CodeBuildRequest | 
        try {
            CodeBuildResponse result = apiInstance.postCodeBuild(codeBuildRequest);
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postCodeBuild");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **codeBuildRequest** | [**CodeBuildRequest**](CodeBuildRequest.md)|  | |

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

## postCodeBuildWithHttpInfo

> ApiResponse<CodeBuildResponse> postCodeBuildWithHttpInfo(codeBuildRequest)

Build a project knowledge index

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        CodeBuildRequest codeBuildRequest = new CodeBuildRequest(); // CodeBuildRequest | 
        try {
            ApiResponse<CodeBuildResponse> response = apiInstance.postCodeBuildWithHttpInfo(codeBuildRequest);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postCodeBuild");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **codeBuildRequest** | [**CodeBuildRequest**](CodeBuildRequest.md)|  | |

### Return type

ApiResponse<[**CodeBuildResponse**](CodeBuildResponse.md)>


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


## postCodeScan

> CodeScanResponse postCodeScan(codeScanRequest)

Request a canonical code index scan

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        CodeScanRequest codeScanRequest = new CodeScanRequest(); // CodeScanRequest | 
        try {
            CodeScanResponse result = apiInstance.postCodeScan(codeScanRequest);
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postCodeScan");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **codeScanRequest** | [**CodeScanRequest**](CodeScanRequest.md)|  | |

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

## postCodeScanWithHttpInfo

> ApiResponse<CodeScanResponse> postCodeScanWithHttpInfo(codeScanRequest)

Request a canonical code index scan

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        CodeScanRequest codeScanRequest = new CodeScanRequest(); // CodeScanRequest | 
        try {
            ApiResponse<CodeScanResponse> response = apiInstance.postCodeScanWithHttpInfo(codeScanRequest);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postCodeScan");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **codeScanRequest** | [**CodeScanRequest**](CodeScanRequest.md)|  | |

### Return type

ApiResponse<[**CodeScanResponse**](CodeScanResponse.md)>


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


## postCodeUpdate

> CodeUpdateResponse postCodeUpdate(codeUpdateRequest)

Incrementally update a project knowledge index

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        CodeUpdateRequest codeUpdateRequest = new CodeUpdateRequest(); // CodeUpdateRequest | 
        try {
            CodeUpdateResponse result = apiInstance.postCodeUpdate(codeUpdateRequest);
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postCodeUpdate");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **codeUpdateRequest** | [**CodeUpdateRequest**](CodeUpdateRequest.md)|  | |

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

## postCodeUpdateWithHttpInfo

> ApiResponse<CodeUpdateResponse> postCodeUpdateWithHttpInfo(codeUpdateRequest)

Incrementally update a project knowledge index

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        CodeUpdateRequest codeUpdateRequest = new CodeUpdateRequest(); // CodeUpdateRequest | 
        try {
            ApiResponse<CodeUpdateResponse> response = apiInstance.postCodeUpdateWithHttpInfo(codeUpdateRequest);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postCodeUpdate");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **codeUpdateRequest** | [**CodeUpdateRequest**](CodeUpdateRequest.md)|  | |

### Return type

ApiResponse<[**CodeUpdateResponse**](CodeUpdateResponse.md)>


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


## postDocs

> DocIngestResponse postDocs(_file, scope)

Upload a document for ingest

Accepts multipart/form-data with a required &#x60;file&#x60; part and an optional &#x60;scope&#x60; field (default: global). Normalizes to markdown, stores in DB2, and queues an extract_doc job. Returns existing doc_id on idempotent re-upload. 

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        File _file = new File("/path/to/file"); // File | 
        String scope = "global"; // String | 
        try {
            DocIngestResponse result = apiInstance.postDocs(_file, scope);
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postDocs");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **_file** | **File**|  | |
| **scope** | **String**|  | [optional] [default to global] |

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

## postDocsWithHttpInfo

> ApiResponse<DocIngestResponse> postDocsWithHttpInfo(_file, scope)

Upload a document for ingest

Accepts multipart/form-data with a required &#x60;file&#x60; part and an optional &#x60;scope&#x60; field (default: global). Normalizes to markdown, stores in DB2, and queues an extract_doc job. Returns existing doc_id on idempotent re-upload. 

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        File _file = new File("/path/to/file"); // File | 
        String scope = "global"; // String | 
        try {
            ApiResponse<DocIngestResponse> response = apiInstance.postDocsWithHttpInfo(_file, scope);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postDocs");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **_file** | **File**|  | |
| **scope** | **String**|  | [optional] [default to global] |

### Return type

ApiResponse<[**DocIngestResponse**](DocIngestResponse.md)>


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


## postDocsManifest

> DocsManifestResponse postDocsManifest(docsManifestRequest)

Check which uploaded document hashes are absent

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        DocsManifestRequest docsManifestRequest = new DocsManifestRequest(); // DocsManifestRequest | 
        try {
            DocsManifestResponse result = apiInstance.postDocsManifest(docsManifestRequest);
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postDocsManifest");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **docsManifestRequest** | [**DocsManifestRequest**](DocsManifestRequest.md)|  | |

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

## postDocsManifestWithHttpInfo

> ApiResponse<DocsManifestResponse> postDocsManifestWithHttpInfo(docsManifestRequest)

Check which uploaded document hashes are absent

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        DocsManifestRequest docsManifestRequest = new DocsManifestRequest(); // DocsManifestRequest | 
        try {
            ApiResponse<DocsManifestResponse> response = apiInstance.postDocsManifestWithHttpInfo(docsManifestRequest);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postDocsManifest");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **docsManifestRequest** | [**DocsManifestRequest**](DocsManifestRequest.md)|  | |

### Return type

ApiResponse<[**DocsManifestResponse**](DocsManifestResponse.md)>


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


## postDrain

> DrainResponse postDrain(drainRequest)

Drain the asynchronous knowledge ingest queue

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        DrainRequest drainRequest = new DrainRequest(); // DrainRequest | 
        try {
            DrainResponse result = apiInstance.postDrain(drainRequest);
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postDrain");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **drainRequest** | [**DrainRequest**](DrainRequest.md)|  | [optional] |

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

## postDrainWithHttpInfo

> ApiResponse<DrainResponse> postDrainWithHttpInfo(drainRequest)

Drain the asynchronous knowledge ingest queue

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        DrainRequest drainRequest = new DrainRequest(); // DrainRequest | 
        try {
            ApiResponse<DrainResponse> response = apiInstance.postDrainWithHttpInfo(drainRequest);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postDrain");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **drainRequest** | [**DrainRequest**](DrainRequest.md)|  | [optional] |

### Return type

ApiResponse<[**DrainResponse**](DrainResponse.md)>


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


## postEntitySearch

> EntitySearchResponse postEntitySearch(entitySearchRequest)

Find entities by name or context

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        EntitySearchRequest entitySearchRequest = new EntitySearchRequest(); // EntitySearchRequest | 
        try {
            EntitySearchResponse result = apiInstance.postEntitySearch(entitySearchRequest);
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postEntitySearch");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **entitySearchRequest** | [**EntitySearchRequest**](EntitySearchRequest.md)|  | |

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

## postEntitySearchWithHttpInfo

> ApiResponse<EntitySearchResponse> postEntitySearchWithHttpInfo(entitySearchRequest)

Find entities by name or context

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        EntitySearchRequest entitySearchRequest = new EntitySearchRequest(); // EntitySearchRequest | 
        try {
            ApiResponse<EntitySearchResponse> response = apiInstance.postEntitySearchWithHttpInfo(entitySearchRequest);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postEntitySearch");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **entitySearchRequest** | [**EntitySearchRequest**](EntitySearchRequest.md)|  | |

### Return type

ApiResponse<[**EntitySearchResponse**](EntitySearchResponse.md)>


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


## postIngest

> IngestResponse postIngest(ingestRequest)

Enqueue background project ingest

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        IngestRequest ingestRequest = new IngestRequest(); // IngestRequest | 
        try {
            IngestResponse result = apiInstance.postIngest(ingestRequest);
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postIngest");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **ingestRequest** | [**IngestRequest**](IngestRequest.md)|  | [optional] |

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

## postIngestWithHttpInfo

> ApiResponse<IngestResponse> postIngestWithHttpInfo(ingestRequest)

Enqueue background project ingest

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        IngestRequest ingestRequest = new IngestRequest(); // IngestRequest | 
        try {
            ApiResponse<IngestResponse> response = apiInstance.postIngestWithHttpInfo(ingestRequest);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postIngest");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **ingestRequest** | [**IngestRequest**](IngestRequest.md)|  | [optional] |

### Return type

ApiResponse<[**IngestResponse**](IngestResponse.md)>


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


## postIntelligenceBanditClose

> Object postIntelligenceBanditClose()

Close a sampled decision with its observed reward

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        try {
            Object result = apiInstance.postIntelligenceBanditClose();
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postIntelligenceBanditClose");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

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

## postIntelligenceBanditCloseWithHttpInfo

> ApiResponse<Object> postIntelligenceBanditCloseWithHttpInfo()

Close a sampled decision with its observed reward

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        try {
            ApiResponse<Object> response = apiInstance.postIntelligenceBanditCloseWithHttpInfo();
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postIntelligenceBanditClose");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters

This endpoint does not need any parameter.

### Return type

ApiResponse<**Object**>


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


## postIntelligenceBanditPromote

> Object postIntelligenceBanditPromote()

Persist the production-default arm for a decision point

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        try {
            Object result = apiInstance.postIntelligenceBanditPromote();
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postIntelligenceBanditPromote");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

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

## postIntelligenceBanditPromoteWithHttpInfo

> ApiResponse<Object> postIntelligenceBanditPromoteWithHttpInfo()

Persist the production-default arm for a decision point

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        try {
            ApiResponse<Object> response = apiInstance.postIntelligenceBanditPromoteWithHttpInfo();
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postIntelligenceBanditPromote");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters

This endpoint does not need any parameter.

### Return type

ApiResponse<**Object**>


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


## postIntelligenceBanditSample

> Object postIntelligenceBanditSample()

Sample an arm for a decision point (server-side decision points)

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        try {
            Object result = apiInstance.postIntelligenceBanditSample();
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postIntelligenceBanditSample");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

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

## postIntelligenceBanditSampleWithHttpInfo

> ApiResponse<Object> postIntelligenceBanditSampleWithHttpInfo()

Sample an arm for a decision point (server-side decision points)

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        try {
            ApiResponse<Object> response = apiInstance.postIntelligenceBanditSampleWithHttpInfo();
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postIntelligenceBanditSample");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters

This endpoint does not need any parameter.

### Return type

ApiResponse<**Object**>


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


## postMaintenanceClear

> MaintenanceClearResponse postMaintenanceClear(maintenanceClearRequest)

Clear indexed knowledge for a project

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        MaintenanceClearRequest maintenanceClearRequest = new MaintenanceClearRequest(); // MaintenanceClearRequest | 
        try {
            MaintenanceClearResponse result = apiInstance.postMaintenanceClear(maintenanceClearRequest);
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postMaintenanceClear");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **maintenanceClearRequest** | [**MaintenanceClearRequest**](MaintenanceClearRequest.md)|  | |

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

## postMaintenanceClearWithHttpInfo

> ApiResponse<MaintenanceClearResponse> postMaintenanceClearWithHttpInfo(maintenanceClearRequest)

Clear indexed knowledge for a project

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        MaintenanceClearRequest maintenanceClearRequest = new MaintenanceClearRequest(); // MaintenanceClearRequest | 
        try {
            ApiResponse<MaintenanceClearResponse> response = apiInstance.postMaintenanceClearWithHttpInfo(maintenanceClearRequest);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postMaintenanceClear");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **maintenanceClearRequest** | [**MaintenanceClearRequest**](MaintenanceClearRequest.md)|  | |

### Return type

ApiResponse<[**MaintenanceClearResponse**](MaintenanceClearResponse.md)>


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


## postMaintenanceReconcile

> MaintenanceReconcileResponse postMaintenanceReconcile(maintenanceReconcileRequest)

Reconcile orphaned vector records

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        MaintenanceReconcileRequest maintenanceReconcileRequest = new MaintenanceReconcileRequest(); // MaintenanceReconcileRequest | 
        try {
            MaintenanceReconcileResponse result = apiInstance.postMaintenanceReconcile(maintenanceReconcileRequest);
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postMaintenanceReconcile");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **maintenanceReconcileRequest** | [**MaintenanceReconcileRequest**](MaintenanceReconcileRequest.md)|  | [optional] |

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

## postMaintenanceReconcileWithHttpInfo

> ApiResponse<MaintenanceReconcileResponse> postMaintenanceReconcileWithHttpInfo(maintenanceReconcileRequest)

Reconcile orphaned vector records

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        MaintenanceReconcileRequest maintenanceReconcileRequest = new MaintenanceReconcileRequest(); // MaintenanceReconcileRequest | 
        try {
            ApiResponse<MaintenanceReconcileResponse> response = apiInstance.postMaintenanceReconcileWithHttpInfo(maintenanceReconcileRequest);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postMaintenanceReconcile");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **maintenanceReconcileRequest** | [**MaintenanceReconcileRequest**](MaintenanceReconcileRequest.md)|  | [optional] |

### Return type

ApiResponse<[**MaintenanceReconcileResponse**](MaintenanceReconcileResponse.md)>


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


## postMaintenanceRepair

> MaintenanceRepairResponse postMaintenanceRepair(maintenanceRepairRequest)

Repair a project knowledge index

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        MaintenanceRepairRequest maintenanceRepairRequest = new MaintenanceRepairRequest(); // MaintenanceRepairRequest | 
        try {
            MaintenanceRepairResponse result = apiInstance.postMaintenanceRepair(maintenanceRepairRequest);
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postMaintenanceRepair");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **maintenanceRepairRequest** | [**MaintenanceRepairRequest**](MaintenanceRepairRequest.md)|  | |

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

## postMaintenanceRepairWithHttpInfo

> ApiResponse<MaintenanceRepairResponse> postMaintenanceRepairWithHttpInfo(maintenanceRepairRequest)

Repair a project knowledge index

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        MaintenanceRepairRequest maintenanceRepairRequest = new MaintenanceRepairRequest(); // MaintenanceRepairRequest | 
        try {
            ApiResponse<MaintenanceRepairResponse> response = apiInstance.postMaintenanceRepairWithHttpInfo(maintenanceRepairRequest);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postMaintenanceRepair");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **maintenanceRepairRequest** | [**MaintenanceRepairRequest**](MaintenanceRepairRequest.md)|  | |

### Return type

ApiResponse<[**MaintenanceRepairResponse**](MaintenanceRepairResponse.md)>


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


## postPromote

> void postPromote(id)

Promote a release to active

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        Long id = 56L; // Long | 
        try {
            apiInstance.postPromote(id);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postPromote");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **id** | **Long**|  | |

### Return type


null (empty response body)

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

## postPromoteWithHttpInfo

> ApiResponse<Void> postPromoteWithHttpInfo(id)

Promote a release to active

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        Long id = 56L; // Long | 
        try {
            ApiResponse<Void> response = apiInstance.postPromoteWithHttpInfo(id);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postPromote");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **id** | **Long**|  | |

### Return type


ApiResponse<Void>

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


## postReleases

> CreateReleaseResponse postReleases(createReleaseRequest)

Create a new corpus release

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        CreateReleaseRequest createReleaseRequest = new CreateReleaseRequest(); // CreateReleaseRequest | 
        try {
            CreateReleaseResponse result = apiInstance.postReleases(createReleaseRequest);
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postReleases");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **createReleaseRequest** | [**CreateReleaseRequest**](CreateReleaseRequest.md)|  | |

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

## postReleasesWithHttpInfo

> ApiResponse<CreateReleaseResponse> postReleasesWithHttpInfo(createReleaseRequest)

Create a new corpus release

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        CreateReleaseRequest createReleaseRequest = new CreateReleaseRequest(); // CreateReleaseRequest | 
        try {
            ApiResponse<CreateReleaseResponse> response = apiInstance.postReleasesWithHttpInfo(createReleaseRequest);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postReleases");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **createReleaseRequest** | [**CreateReleaseRequest**](CreateReleaseRequest.md)|  | |

### Return type

ApiResponse<[**CreateReleaseResponse**](CreateReleaseResponse.md)>


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


## postReviewAccept

> void postReviewAccept(id, reviewAcceptRequest)

Accept a staged document

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        Long id = 56L; // Long | 
        ReviewAcceptRequest reviewAcceptRequest = new ReviewAcceptRequest(); // ReviewAcceptRequest | 
        try {
            apiInstance.postReviewAccept(id, reviewAcceptRequest);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postReviewAccept");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **id** | **Long**|  | |
| **reviewAcceptRequest** | [**ReviewAcceptRequest**](ReviewAcceptRequest.md)|  | [optional] |

### Return type


null (empty response body)

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

## postReviewAcceptWithHttpInfo

> ApiResponse<Void> postReviewAcceptWithHttpInfo(id, reviewAcceptRequest)

Accept a staged document

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        Long id = 56L; // Long | 
        ReviewAcceptRequest reviewAcceptRequest = new ReviewAcceptRequest(); // ReviewAcceptRequest | 
        try {
            ApiResponse<Void> response = apiInstance.postReviewAcceptWithHttpInfo(id, reviewAcceptRequest);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postReviewAccept");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **id** | **Long**|  | |
| **reviewAcceptRequest** | [**ReviewAcceptRequest**](ReviewAcceptRequest.md)|  | [optional] |

### Return type


ApiResponse<Void>

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


## postReviewReject

> void postReviewReject(id, reviewRejectRequest)

Reject a staged document

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        Long id = 56L; // Long | 
        ReviewRejectRequest reviewRejectRequest = new ReviewRejectRequest(); // ReviewRejectRequest | 
        try {
            apiInstance.postReviewReject(id, reviewRejectRequest);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postReviewReject");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **id** | **Long**|  | |
| **reviewRejectRequest** | [**ReviewRejectRequest**](ReviewRejectRequest.md)|  | |

### Return type


null (empty response body)

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

## postReviewRejectWithHttpInfo

> ApiResponse<Void> postReviewRejectWithHttpInfo(id, reviewRejectRequest)

Reject a staged document

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        Long id = 56L; // Long | 
        ReviewRejectRequest reviewRejectRequest = new ReviewRejectRequest(); // ReviewRejectRequest | 
        try {
            ApiResponse<Void> response = apiInstance.postReviewRejectWithHttpInfo(id, reviewRejectRequest);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postReviewReject");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **id** | **Long**|  | |
| **reviewRejectRequest** | [**ReviewRejectRequest**](ReviewRejectRequest.md)|  | |

### Return type


ApiResponse<Void>

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


## postRollback

> void postRollback(id, rollbackRequest)

Roll back to a prior release

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        Long id = 56L; // Long | 
        RollbackRequest rollbackRequest = new RollbackRequest(); // RollbackRequest | 
        try {
            apiInstance.postRollback(id, rollbackRequest);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postRollback");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **id** | **Long**|  | |
| **rollbackRequest** | [**RollbackRequest**](RollbackRequest.md)|  | [optional] |

### Return type


null (empty response body)

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

## postRollbackWithHttpInfo

> ApiResponse<Void> postRollbackWithHttpInfo(id, rollbackRequest)

Roll back to a prior release

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        Long id = 56L; // Long | 
        RollbackRequest rollbackRequest = new RollbackRequest(); // RollbackRequest | 
        try {
            ApiResponse<Void> response = apiInstance.postRollbackWithHttpInfo(id, rollbackRequest);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postRollback");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **id** | **Long**|  | |
| **rollbackRequest** | [**RollbackRequest**](RollbackRequest.md)|  | [optional] |

### Return type


ApiResponse<Void>

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


## postSearch

> SearchResponse postSearch(searchRequest)

Hybrid knowledge search

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        SearchRequest searchRequest = new SearchRequest(); // SearchRequest | 
        try {
            SearchResponse result = apiInstance.postSearch(searchRequest);
            System.out.println(result);
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postSearch");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **searchRequest** | [**SearchRequest**](SearchRequest.md)|  | |

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

## postSearchWithHttpInfo

> ApiResponse<SearchResponse> postSearchWithHttpInfo(searchRequest)

Hybrid knowledge search

### Example

```java
// Import classes:
import org.openapitools.client.ApiClient;
import org.openapitools.client.ApiException;
import org.openapitools.client.ApiResponse;
import org.openapitools.client.Configuration;
import org.openapitools.client.auth.*;
import org.openapitools.client.models.*;
import org.openapitools.client.api.DefaultApi;

public class Example {
    public static void main(String[] args) {
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://127.0.0.1:8090/v1");
        
        // Configure HTTP bearer authorization: bearerAuth
        HttpBearerAuth bearerAuth = (HttpBearerAuth) defaultClient.getAuthentication("bearerAuth");
        bearerAuth.setBearerToken("BEARER TOKEN");

        DefaultApi apiInstance = new DefaultApi(defaultClient);
        SearchRequest searchRequest = new SearchRequest(); // SearchRequest | 
        try {
            ApiResponse<SearchResponse> response = apiInstance.postSearchWithHttpInfo(searchRequest);
            System.out.println("Status code: " + response.getStatusCode());
            System.out.println("Response headers: " + response.getHeaders());
            System.out.println("Response body: " + response.getData());
        } catch (ApiException e) {
            System.err.println("Exception when calling DefaultApi#postSearch");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Response headers: " + e.getResponseHeaders());
            System.err.println("Reason: " + e.getResponseBody());
            e.printStackTrace();
        }
    }
}
```

### Parameters


| Name | Type | Description  | Notes |
|------------- | ------------- | ------------- | -------------|
| **searchRequest** | [**SearchRequest**](SearchRequest.md)|  | |

### Return type

ApiResponse<[**SearchResponse**](SearchResponse.md)>


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

