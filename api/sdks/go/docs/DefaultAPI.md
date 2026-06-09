# \DefaultAPI

All URIs are relative to *http://127.0.0.1:8090/v1*

Method | HTTP request | Description
------------- | ------------- | -------------
[**DeleteDoc**](DefaultAPI.md#DeleteDoc) | **Delete** /docs/{id} | Delete a staged document
[**GetActiveRelease**](DefaultAPI.md#GetActiveRelease) | **Get** /releases/active | Get the currently active corpus release
[**GetArtifact**](DefaultAPI.md#GetArtifact) | **Get** /artifacts/{id} | Retrieve an artifact by UUID
[**GetArtifactLinks**](DefaultAPI.md#GetArtifactLinks) | **Get** /artifacts/{id}/links | Retrieve outgoing links from an artifact
[**GetCapabilities**](DefaultAPI.md#GetCapabilities) | **Get** /capabilities | Advertised capabilities
[**GetCodeBlastRadius**](DefaultAPI.md#GetCodeBlastRadius) | **Get** /code/blast-radius | Blast-radius computation for a file
[**GetCodeCallers**](DefaultAPI.md#GetCodeCallers) | **Get** /code/callers | Call sites for a symbol in the canonical code index
[**GetCodeFind**](DefaultAPI.md#GetCodeFind) | **Get** /code/find | Symbol/identifier lookup across the canonical index
[**GetCodeProjectStats**](DefaultAPI.md#GetCodeProjectStats) | **Get** /code/project-stats | Project-level canonical code index counts and language breakdown
[**GetCodeProjects**](DefaultAPI.md#GetCodeProjects) | **Get** /code/projects | List projects in the canonical code index
[**GetCodeSearch**](DefaultAPI.md#GetCodeSearch) | **Get** /code/search | Full-text code search across indexed file contents
[**GetCodeStructure**](DefaultAPI.md#GetCodeStructure) | **Get** /code/structure | Definitions for a file in the canonical code index
[**GetDoc**](DefaultAPI.md#GetDoc) | **Get** /docs/{id} | Retrieve doc metadata by id
[**GetEntityProfile**](DefaultAPI.md#GetEntityProfile) | **Get** /entities/{id} | Canonical entity profile
[**GetHealth**](DefaultAPI.md#GetHealth) | **Get** /health | Service health check
[**GetIngestStatus**](DefaultAPI.md#GetIngestStatus) | **Get** /ingest/status | Report background project ingest status
[**GetIntelligenceBanditExport**](DefaultAPI.md#GetIntelligenceBanditExport) | **Get** /intelligence/bandit/export | Export fusion bandit decision data
[**GetIntelligenceCalibrationReadiness**](DefaultAPI.md#GetIntelligenceCalibrationReadiness) | **Get** /intelligence/calibration/readiness | Calibration readiness
[**GetIntelligenceDemotionCheck**](DefaultAPI.md#GetIntelligenceDemotionCheck) | **Get** /intelligence/demotion/check | Dry-run demotion readiness check
[**GetJobStatus**](DefaultAPI.md#GetJobStatus) | **Get** /jobs/{job_id} | Report asynchronous knowledge ingest job status
[**GetPipelineStatus**](DefaultAPI.md#GetPipelineStatus) | **Get** /pipeline/status | Report asynchronous knowledge ingest queue status
[**GetReview**](DefaultAPI.md#GetReview) | **Get** /review | List staged documents pending review
[**GetVersion**](DefaultAPI.md#GetVersion) | **Get** /version | Service version
[**GetWorkers**](DefaultAPI.md#GetWorkers) | **Get** /workers | Report aimee-kb worker and background task status
[**HeadHealth**](DefaultAPI.md#HeadHealth) | **Head** /health | Service health check (HEAD)
[**PostAction**](DefaultAPI.md#PostAction) | **Post** /actions/{action} | Execute a versioned knowledge-service action
[**PostCodeBuild**](DefaultAPI.md#PostCodeBuild) | **Post** /code/build | Build a project knowledge index
[**PostCodeScan**](DefaultAPI.md#PostCodeScan) | **Post** /code/scan | Request a canonical code index scan
[**PostCodeUpdate**](DefaultAPI.md#PostCodeUpdate) | **Post** /code/update | Incrementally update a project knowledge index
[**PostDocs**](DefaultAPI.md#PostDocs) | **Post** /docs | Upload a document for ingest
[**PostDocsManifest**](DefaultAPI.md#PostDocsManifest) | **Post** /docs/manifest | Check which uploaded document hashes are absent
[**PostDrain**](DefaultAPI.md#PostDrain) | **Post** /drain | Drain the asynchronous knowledge ingest queue
[**PostEntitySearch**](DefaultAPI.md#PostEntitySearch) | **Post** /entities/search | Find entities by name or context
[**PostIngest**](DefaultAPI.md#PostIngest) | **Post** /ingest | Enqueue background project ingest
[**PostIntelligenceBanditClose**](DefaultAPI.md#PostIntelligenceBanditClose) | **Post** /intelligence/bandit/close | Close a sampled decision with its observed reward
[**PostIntelligenceBanditPromote**](DefaultAPI.md#PostIntelligenceBanditPromote) | **Post** /intelligence/bandit/promote | Persist the production-default arm for a decision point
[**PostIntelligenceBanditSample**](DefaultAPI.md#PostIntelligenceBanditSample) | **Post** /intelligence/bandit/sample | Sample an arm for a decision point (server-side decision points)
[**PostMaintenanceClear**](DefaultAPI.md#PostMaintenanceClear) | **Post** /maintenance/clear | Clear indexed knowledge for a project
[**PostMaintenanceReconcile**](DefaultAPI.md#PostMaintenanceReconcile) | **Post** /maintenance/reconcile | Reconcile orphaned vector records
[**PostMaintenanceRepair**](DefaultAPI.md#PostMaintenanceRepair) | **Post** /maintenance/repair | Repair a project knowledge index
[**PostPromote**](DefaultAPI.md#PostPromote) | **Post** /releases/{id}/promote | Promote a release to active
[**PostReleases**](DefaultAPI.md#PostReleases) | **Post** /releases | Create a new corpus release
[**PostReviewAccept**](DefaultAPI.md#PostReviewAccept) | **Post** /review/{id}/accept | Accept a staged document
[**PostReviewReject**](DefaultAPI.md#PostReviewReject) | **Post** /review/{id}/reject | Reject a staged document
[**PostRollback**](DefaultAPI.md#PostRollback) | **Post** /releases/{id}/rollback | Roll back to a prior release
[**PostSearch**](DefaultAPI.md#PostSearch) | **Post** /search | Hybrid knowledge search



## DeleteDoc

> DeleteDoc(ctx, id).Execute()

Delete a staged document

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	id := int64(789) // int64 | 

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	r, err := apiClient.DefaultAPI.DeleteDoc(context.Background(), id).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.DeleteDoc``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
}
```

### Path Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**ctx** | **context.Context** | context for authentication, logging, cancellation, deadlines, tracing, etc.
**id** | **int64** |  | 

### Other Parameters

Other parameters are passed through a pointer to a apiDeleteDocRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------


### Return type

 (empty response body)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: Not defined

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## GetActiveRelease

> ActiveReleaseResponse GetActiveRelease(ctx).Execute()

Get the currently active corpus release

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.GetActiveRelease(context.Background()).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.GetActiveRelease``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `GetActiveRelease`: ActiveReleaseResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.GetActiveRelease`: %v\n", resp)
}
```

### Path Parameters

This endpoint does not need any parameter.

### Other Parameters

Other parameters are passed through a pointer to a apiGetActiveReleaseRequest struct via the builder pattern


### Return type

[**ActiveReleaseResponse**](ActiveReleaseResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## GetArtifact

> ArtifactResponse GetArtifact(ctx, id).Execute()

Retrieve an artifact by UUID

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	id := "id_example" // string | 

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.GetArtifact(context.Background(), id).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.GetArtifact``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `GetArtifact`: ArtifactResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.GetArtifact`: %v\n", resp)
}
```

### Path Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**ctx** | **context.Context** | context for authentication, logging, cancellation, deadlines, tracing, etc.
**id** | **string** |  | 

### Other Parameters

Other parameters are passed through a pointer to a apiGetArtifactRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------


### Return type

[**ArtifactResponse**](ArtifactResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## GetArtifactLinks

> ArtifactLinksResponse GetArtifactLinks(ctx, id).Execute()

Retrieve outgoing links from an artifact

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	id := "id_example" // string | 

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.GetArtifactLinks(context.Background(), id).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.GetArtifactLinks``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `GetArtifactLinks`: ArtifactLinksResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.GetArtifactLinks`: %v\n", resp)
}
```

### Path Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**ctx** | **context.Context** | context for authentication, logging, cancellation, deadlines, tracing, etc.
**id** | **string** |  | 

### Other Parameters

Other parameters are passed through a pointer to a apiGetArtifactLinksRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------


### Return type

[**ArtifactLinksResponse**](ArtifactLinksResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## GetCapabilities

> CapabilitiesResponse GetCapabilities(ctx).Execute()

Advertised capabilities



### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.GetCapabilities(context.Background()).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.GetCapabilities``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `GetCapabilities`: CapabilitiesResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.GetCapabilities`: %v\n", resp)
}
```

### Path Parameters

This endpoint does not need any parameter.

### Other Parameters

Other parameters are passed through a pointer to a apiGetCapabilitiesRequest struct via the builder pattern


### Return type

[**CapabilitiesResponse**](CapabilitiesResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## GetCodeBlastRadius

> BlastRadiusResponse GetCodeBlastRadius(ctx).Project(project).FilePath(filePath).Execute()

Blast-radius computation for a file

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	project := "project_example" // string | 
	filePath := "filePath_example" // string |  (optional)

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.GetCodeBlastRadius(context.Background()).Project(project).FilePath(filePath).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.GetCodeBlastRadius``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `GetCodeBlastRadius`: BlastRadiusResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.GetCodeBlastRadius`: %v\n", resp)
}
```

### Path Parameters



### Other Parameters

Other parameters are passed through a pointer to a apiGetCodeBlastRadiusRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **project** | **string** |  | 
 **filePath** | **string** |  | 

### Return type

[**BlastRadiusResponse**](BlastRadiusResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## GetCodeCallers

> CodeCallersResponse GetCodeCallers(ctx).Symbol(symbol).Project(project).MaxResults(maxResults).Execute()

Call sites for a symbol in the canonical code index

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	symbol := "symbol_example" // string | 
	project := "project_example" // string |  (optional)
	maxResults := int32(56) // int32 |  (optional) (default to 20)

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.GetCodeCallers(context.Background()).Symbol(symbol).Project(project).MaxResults(maxResults).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.GetCodeCallers``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `GetCodeCallers`: CodeCallersResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.GetCodeCallers`: %v\n", resp)
}
```

### Path Parameters



### Other Parameters

Other parameters are passed through a pointer to a apiGetCodeCallersRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **symbol** | **string** |  | 
 **project** | **string** |  | 
 **maxResults** | **int32** |  | [default to 20]

### Return type

[**CodeCallersResponse**](CodeCallersResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## GetCodeFind

> CodeFindResponse GetCodeFind(ctx).Identifier(identifier).Project(project).MaxResults(maxResults).Execute()

Symbol/identifier lookup across the canonical index

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	identifier := "identifier_example" // string | 
	project := "project_example" // string |  (optional)
	maxResults := int32(56) // int32 |  (optional) (default to 20)

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.GetCodeFind(context.Background()).Identifier(identifier).Project(project).MaxResults(maxResults).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.GetCodeFind``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `GetCodeFind`: CodeFindResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.GetCodeFind`: %v\n", resp)
}
```

### Path Parameters



### Other Parameters

Other parameters are passed through a pointer to a apiGetCodeFindRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **identifier** | **string** |  | 
 **project** | **string** |  | 
 **maxResults** | **int32** |  | [default to 20]

### Return type

[**CodeFindResponse**](CodeFindResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## GetCodeProjectStats

> CodeProjectStatsResponse GetCodeProjectStats(ctx).Project(project).Execute()

Project-level canonical code index counts and language breakdown

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	project := "project_example" // string | 

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.GetCodeProjectStats(context.Background()).Project(project).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.GetCodeProjectStats``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `GetCodeProjectStats`: CodeProjectStatsResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.GetCodeProjectStats`: %v\n", resp)
}
```

### Path Parameters



### Other Parameters

Other parameters are passed through a pointer to a apiGetCodeProjectStatsRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **project** | **string** |  | 

### Return type

[**CodeProjectStatsResponse**](CodeProjectStatsResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## GetCodeProjects

> CodeProjectsResponse GetCodeProjects(ctx).MaxResults(maxResults).Execute()

List projects in the canonical code index

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	maxResults := int32(56) // int32 |  (optional) (default to 100)

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.GetCodeProjects(context.Background()).MaxResults(maxResults).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.GetCodeProjects``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `GetCodeProjects`: CodeProjectsResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.GetCodeProjects`: %v\n", resp)
}
```

### Path Parameters



### Other Parameters

Other parameters are passed through a pointer to a apiGetCodeProjectsRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **maxResults** | **int32** |  | [default to 100]

### Return type

[**CodeProjectsResponse**](CodeProjectsResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## GetCodeSearch

> CodeSearchResponse GetCodeSearch(ctx).Query(query).Project(project).MaxResults(maxResults).Execute()

Full-text code search across indexed file contents

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	query := "query_example" // string | 
	project := "project_example" // string |  (optional)
	maxResults := int32(56) // int32 |  (optional) (default to 20)

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.GetCodeSearch(context.Background()).Query(query).Project(project).MaxResults(maxResults).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.GetCodeSearch``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `GetCodeSearch`: CodeSearchResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.GetCodeSearch`: %v\n", resp)
}
```

### Path Parameters



### Other Parameters

Other parameters are passed through a pointer to a apiGetCodeSearchRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **query** | **string** |  | 
 **project** | **string** |  | 
 **maxResults** | **int32** |  | [default to 20]

### Return type

[**CodeSearchResponse**](CodeSearchResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## GetCodeStructure

> CodeStructureResponse GetCodeStructure(ctx).Project(project).FilePath(filePath).MaxResults(maxResults).Execute()

Definitions for a file in the canonical code index

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	project := "project_example" // string | 
	filePath := "filePath_example" // string | 
	maxResults := int32(56) // int32 |  (optional) (default to 256)

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.GetCodeStructure(context.Background()).Project(project).FilePath(filePath).MaxResults(maxResults).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.GetCodeStructure``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `GetCodeStructure`: CodeStructureResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.GetCodeStructure`: %v\n", resp)
}
```

### Path Parameters



### Other Parameters

Other parameters are passed through a pointer to a apiGetCodeStructureRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **project** | **string** |  | 
 **filePath** | **string** |  | 
 **maxResults** | **int32** |  | [default to 256]

### Return type

[**CodeStructureResponse**](CodeStructureResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## GetDoc

> DocMetadataResponse GetDoc(ctx, id).Execute()

Retrieve doc metadata by id

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	id := int64(789) // int64 | 

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.GetDoc(context.Background(), id).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.GetDoc``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `GetDoc`: DocMetadataResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.GetDoc`: %v\n", resp)
}
```

### Path Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**ctx** | **context.Context** | context for authentication, logging, cancellation, deadlines, tracing, etc.
**id** | **int64** |  | 

### Other Parameters

Other parameters are passed through a pointer to a apiGetDocRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------


### Return type

[**DocMetadataResponse**](DocMetadataResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## GetEntityProfile

> EntityProfileResponse GetEntityProfile(ctx, id).Execute()

Canonical entity profile

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	id := "id_example" // string | Entity name (slug)

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.GetEntityProfile(context.Background(), id).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.GetEntityProfile``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `GetEntityProfile`: EntityProfileResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.GetEntityProfile`: %v\n", resp)
}
```

### Path Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**ctx** | **context.Context** | context for authentication, logging, cancellation, deadlines, tracing, etc.
**id** | **string** | Entity name (slug) | 

### Other Parameters

Other parameters are passed through a pointer to a apiGetEntityProfileRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------


### Return type

[**EntityProfileResponse**](EntityProfileResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## GetHealth

> HealthResponse GetHealth(ctx).Execute()

Service health check



### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.GetHealth(context.Background()).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.GetHealth``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `GetHealth`: HealthResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.GetHealth`: %v\n", resp)
}
```

### Path Parameters

This endpoint does not need any parameter.

### Other Parameters

Other parameters are passed through a pointer to a apiGetHealthRequest struct via the builder pattern


### Return type

[**HealthResponse**](HealthResponse.md)

### Authorization

No authorization required

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## GetIngestStatus

> IngestStatusResponse GetIngestStatus(ctx).Execute()

Report background project ingest status

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.GetIngestStatus(context.Background()).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.GetIngestStatus``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `GetIngestStatus`: IngestStatusResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.GetIngestStatus`: %v\n", resp)
}
```

### Path Parameters

This endpoint does not need any parameter.

### Other Parameters

Other parameters are passed through a pointer to a apiGetIngestStatusRequest struct via the builder pattern


### Return type

[**IngestStatusResponse**](IngestStatusResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## GetIntelligenceBanditExport

> map[string]interface{} GetIntelligenceBanditExport(ctx).Execute()

Export fusion bandit decision data

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.GetIntelligenceBanditExport(context.Background()).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.GetIntelligenceBanditExport``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `GetIntelligenceBanditExport`: map[string]interface{}
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.GetIntelligenceBanditExport`: %v\n", resp)
}
```

### Path Parameters

This endpoint does not need any parameter.

### Other Parameters

Other parameters are passed through a pointer to a apiGetIntelligenceBanditExportRequest struct via the builder pattern


### Return type

**map[string]interface{}**

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## GetIntelligenceCalibrationReadiness

> map[string]interface{} GetIntelligenceCalibrationReadiness(ctx).Execute()

Calibration readiness

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.GetIntelligenceCalibrationReadiness(context.Background()).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.GetIntelligenceCalibrationReadiness``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `GetIntelligenceCalibrationReadiness`: map[string]interface{}
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.GetIntelligenceCalibrationReadiness`: %v\n", resp)
}
```

### Path Parameters

This endpoint does not need any parameter.

### Other Parameters

Other parameters are passed through a pointer to a apiGetIntelligenceCalibrationReadinessRequest struct via the builder pattern


### Return type

**map[string]interface{}**

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## GetIntelligenceDemotionCheck

> map[string]interface{} GetIntelligenceDemotionCheck(ctx).Execute()

Dry-run demotion readiness check

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.GetIntelligenceDemotionCheck(context.Background()).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.GetIntelligenceDemotionCheck``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `GetIntelligenceDemotionCheck`: map[string]interface{}
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.GetIntelligenceDemotionCheck`: %v\n", resp)
}
```

### Path Parameters

This endpoint does not need any parameter.

### Other Parameters

Other parameters are passed through a pointer to a apiGetIntelligenceDemotionCheckRequest struct via the builder pattern


### Return type

**map[string]interface{}**

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## GetJobStatus

> JobStatusResponse GetJobStatus(ctx, jobId).Execute()

Report asynchronous knowledge ingest job status

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	jobId := int64(789) // int64 | 

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.GetJobStatus(context.Background(), jobId).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.GetJobStatus``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `GetJobStatus`: JobStatusResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.GetJobStatus`: %v\n", resp)
}
```

### Path Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**ctx** | **context.Context** | context for authentication, logging, cancellation, deadlines, tracing, etc.
**jobId** | **int64** |  | 

### Other Parameters

Other parameters are passed through a pointer to a apiGetJobStatusRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------


### Return type

[**JobStatusResponse**](JobStatusResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## GetPipelineStatus

> PipelineStatusResponse GetPipelineStatus(ctx).Execute()

Report asynchronous knowledge ingest queue status

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.GetPipelineStatus(context.Background()).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.GetPipelineStatus``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `GetPipelineStatus`: PipelineStatusResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.GetPipelineStatus`: %v\n", resp)
}
```

### Path Parameters

This endpoint does not need any parameter.

### Other Parameters

Other parameters are passed through a pointer to a apiGetPipelineStatusRequest struct via the builder pattern


### Return type

[**PipelineStatusResponse**](PipelineStatusResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## GetReview

> ReviewQueueResponse GetReview(ctx).Cursor(cursor).Limit(limit).Execute()

List staged documents pending review

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	cursor := int64(789) // int64 |  (optional)
	limit := int32(56) // int32 |  (optional) (default to 10)

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.GetReview(context.Background()).Cursor(cursor).Limit(limit).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.GetReview``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `GetReview`: ReviewQueueResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.GetReview`: %v\n", resp)
}
```

### Path Parameters



### Other Parameters

Other parameters are passed through a pointer to a apiGetReviewRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **cursor** | **int64** |  | 
 **limit** | **int32** |  | [default to 10]

### Return type

[**ReviewQueueResponse**](ReviewQueueResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## GetVersion

> VersionResponse GetVersion(ctx).Execute()

Service version

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.GetVersion(context.Background()).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.GetVersion``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `GetVersion`: VersionResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.GetVersion`: %v\n", resp)
}
```

### Path Parameters

This endpoint does not need any parameter.

### Other Parameters

Other parameters are passed through a pointer to a apiGetVersionRequest struct via the builder pattern


### Return type

[**VersionResponse**](VersionResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## GetWorkers

> WorkersResponse GetWorkers(ctx).Execute()

Report aimee-kb worker and background task status

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.GetWorkers(context.Background()).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.GetWorkers``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `GetWorkers`: WorkersResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.GetWorkers`: %v\n", resp)
}
```

### Path Parameters

This endpoint does not need any parameter.

### Other Parameters

Other parameters are passed through a pointer to a apiGetWorkersRequest struct via the builder pattern


### Return type

[**WorkersResponse**](WorkersResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## HeadHealth

> HeadHealth(ctx).Execute()

Service health check (HEAD)

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	r, err := apiClient.DefaultAPI.HeadHealth(context.Background()).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.HeadHealth``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
}
```

### Path Parameters

This endpoint does not need any parameter.

### Other Parameters

Other parameters are passed through a pointer to a apiHeadHealthRequest struct via the builder pattern


### Return type

 (empty response body)

### Authorization

No authorization required

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: Not defined

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## PostAction

> map[string]interface{} PostAction(ctx, action).Body(body).Execute()

Execute a versioned knowledge-service action

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	action := "action_example" // string | 
	body := map[string]interface{}{ ... } // map[string]interface{} |  (optional)

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.PostAction(context.Background(), action).Body(body).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.PostAction``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `PostAction`: map[string]interface{}
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.PostAction`: %v\n", resp)
}
```

### Path Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**ctx** | **context.Context** | context for authentication, logging, cancellation, deadlines, tracing, etc.
**action** | **string** |  | 

### Other Parameters

Other parameters are passed through a pointer to a apiPostActionRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------

 **body** | **map[string]interface{}** |  | 

### Return type

**map[string]interface{}**

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## PostCodeBuild

> CodeBuildResponse PostCodeBuild(ctx).CodeBuildRequest(codeBuildRequest).Execute()

Build a project knowledge index

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	codeBuildRequest := *openapiclient.NewCodeBuildRequest("Path_example", "Project_example") // CodeBuildRequest | 

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.PostCodeBuild(context.Background()).CodeBuildRequest(codeBuildRequest).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.PostCodeBuild``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `PostCodeBuild`: CodeBuildResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.PostCodeBuild`: %v\n", resp)
}
```

### Path Parameters



### Other Parameters

Other parameters are passed through a pointer to a apiPostCodeBuildRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **codeBuildRequest** | [**CodeBuildRequest**](CodeBuildRequest.md) |  | 

### Return type

[**CodeBuildResponse**](CodeBuildResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## PostCodeScan

> CodeScanResponse PostCodeScan(ctx).CodeScanRequest(codeScanRequest).Execute()

Request a canonical code index scan

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	codeScanRequest := *openapiclient.NewCodeScanRequest("Project_example", "RootPath_example") // CodeScanRequest | 

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.PostCodeScan(context.Background()).CodeScanRequest(codeScanRequest).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.PostCodeScan``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `PostCodeScan`: CodeScanResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.PostCodeScan`: %v\n", resp)
}
```

### Path Parameters



### Other Parameters

Other parameters are passed through a pointer to a apiPostCodeScanRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **codeScanRequest** | [**CodeScanRequest**](CodeScanRequest.md) |  | 

### Return type

[**CodeScanResponse**](CodeScanResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## PostCodeUpdate

> CodeUpdateResponse PostCodeUpdate(ctx).CodeUpdateRequest(codeUpdateRequest).Execute()

Incrementally update a project knowledge index

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	codeUpdateRequest := *openapiclient.NewCodeUpdateRequest("Path_example", "Project_example") // CodeUpdateRequest | 

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.PostCodeUpdate(context.Background()).CodeUpdateRequest(codeUpdateRequest).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.PostCodeUpdate``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `PostCodeUpdate`: CodeUpdateResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.PostCodeUpdate`: %v\n", resp)
}
```

### Path Parameters



### Other Parameters

Other parameters are passed through a pointer to a apiPostCodeUpdateRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **codeUpdateRequest** | [**CodeUpdateRequest**](CodeUpdateRequest.md) |  | 

### Return type

[**CodeUpdateResponse**](CodeUpdateResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## PostDocs

> DocIngestResponse PostDocs(ctx).File(file).Scope(scope).Execute()

Upload a document for ingest



### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	file := os.NewFile(1234, "some_file") // *os.File | 
	scope := "scope_example" // string |  (optional) (default to "global")

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.PostDocs(context.Background()).File(file).Scope(scope).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.PostDocs``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `PostDocs`: DocIngestResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.PostDocs`: %v\n", resp)
}
```

### Path Parameters



### Other Parameters

Other parameters are passed through a pointer to a apiPostDocsRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **file** | ***os.File** |  | 
 **scope** | **string** |  | [default to &quot;global&quot;]

### Return type

[**DocIngestResponse**](DocIngestResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: multipart/form-data
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## PostDocsManifest

> DocsManifestResponse PostDocsManifest(ctx).DocsManifestRequest(docsManifestRequest).Execute()

Check which uploaded document hashes are absent

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	docsManifestRequest := *openapiclient.NewDocsManifestRequest([]openapiclient.DocsManifestRequestDocsInner{*openapiclient.NewDocsManifestRequestDocsInner("ContentHash_example")}) // DocsManifestRequest | 

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.PostDocsManifest(context.Background()).DocsManifestRequest(docsManifestRequest).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.PostDocsManifest``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `PostDocsManifest`: DocsManifestResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.PostDocsManifest`: %v\n", resp)
}
```

### Path Parameters



### Other Parameters

Other parameters are passed through a pointer to a apiPostDocsManifestRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **docsManifestRequest** | [**DocsManifestRequest**](DocsManifestRequest.md) |  | 

### Return type

[**DocsManifestResponse**](DocsManifestResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## PostDrain

> DrainResponse PostDrain(ctx).DrainRequest(drainRequest).Execute()

Drain the asynchronous knowledge ingest queue

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	drainRequest := *openapiclient.NewDrainRequest() // DrainRequest |  (optional)

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.PostDrain(context.Background()).DrainRequest(drainRequest).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.PostDrain``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `PostDrain`: DrainResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.PostDrain`: %v\n", resp)
}
```

### Path Parameters



### Other Parameters

Other parameters are passed through a pointer to a apiPostDrainRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **drainRequest** | [**DrainRequest**](DrainRequest.md) |  | 

### Return type

[**DrainResponse**](DrainResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## PostEntitySearch

> EntitySearchResponse PostEntitySearch(ctx).EntitySearchRequest(entitySearchRequest).Execute()

Find entities by name or context

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	entitySearchRequest := *openapiclient.NewEntitySearchRequest("Query_example") // EntitySearchRequest | 

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.PostEntitySearch(context.Background()).EntitySearchRequest(entitySearchRequest).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.PostEntitySearch``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `PostEntitySearch`: EntitySearchResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.PostEntitySearch`: %v\n", resp)
}
```

### Path Parameters



### Other Parameters

Other parameters are passed through a pointer to a apiPostEntitySearchRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **entitySearchRequest** | [**EntitySearchRequest**](EntitySearchRequest.md) |  | 

### Return type

[**EntitySearchResponse**](EntitySearchResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## PostIngest

> IngestResponse PostIngest(ctx).IngestRequest(ingestRequest).Execute()

Enqueue background project ingest

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	ingestRequest := *openapiclient.NewIngestRequest() // IngestRequest |  (optional)

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.PostIngest(context.Background()).IngestRequest(ingestRequest).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.PostIngest``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `PostIngest`: IngestResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.PostIngest`: %v\n", resp)
}
```

### Path Parameters



### Other Parameters

Other parameters are passed through a pointer to a apiPostIngestRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **ingestRequest** | [**IngestRequest**](IngestRequest.md) |  | 

### Return type

[**IngestResponse**](IngestResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## PostIntelligenceBanditClose

> map[string]interface{} PostIntelligenceBanditClose(ctx).Execute()

Close a sampled decision with its observed reward

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.PostIntelligenceBanditClose(context.Background()).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.PostIntelligenceBanditClose``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `PostIntelligenceBanditClose`: map[string]interface{}
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.PostIntelligenceBanditClose`: %v\n", resp)
}
```

### Path Parameters

This endpoint does not need any parameter.

### Other Parameters

Other parameters are passed through a pointer to a apiPostIntelligenceBanditCloseRequest struct via the builder pattern


### Return type

**map[string]interface{}**

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## PostIntelligenceBanditPromote

> map[string]interface{} PostIntelligenceBanditPromote(ctx).Execute()

Persist the production-default arm for a decision point

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.PostIntelligenceBanditPromote(context.Background()).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.PostIntelligenceBanditPromote``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `PostIntelligenceBanditPromote`: map[string]interface{}
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.PostIntelligenceBanditPromote`: %v\n", resp)
}
```

### Path Parameters

This endpoint does not need any parameter.

### Other Parameters

Other parameters are passed through a pointer to a apiPostIntelligenceBanditPromoteRequest struct via the builder pattern


### Return type

**map[string]interface{}**

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## PostIntelligenceBanditSample

> map[string]interface{} PostIntelligenceBanditSample(ctx).Execute()

Sample an arm for a decision point (server-side decision points)

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.PostIntelligenceBanditSample(context.Background()).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.PostIntelligenceBanditSample``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `PostIntelligenceBanditSample`: map[string]interface{}
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.PostIntelligenceBanditSample`: %v\n", resp)
}
```

### Path Parameters

This endpoint does not need any parameter.

### Other Parameters

Other parameters are passed through a pointer to a apiPostIntelligenceBanditSampleRequest struct via the builder pattern


### Return type

**map[string]interface{}**

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## PostMaintenanceClear

> MaintenanceClearResponse PostMaintenanceClear(ctx).MaintenanceClearRequest(maintenanceClearRequest).Execute()

Clear indexed knowledge for a project

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	maintenanceClearRequest := *openapiclient.NewMaintenanceClearRequest("Project_example") // MaintenanceClearRequest | 

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.PostMaintenanceClear(context.Background()).MaintenanceClearRequest(maintenanceClearRequest).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.PostMaintenanceClear``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `PostMaintenanceClear`: MaintenanceClearResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.PostMaintenanceClear`: %v\n", resp)
}
```

### Path Parameters



### Other Parameters

Other parameters are passed through a pointer to a apiPostMaintenanceClearRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **maintenanceClearRequest** | [**MaintenanceClearRequest**](MaintenanceClearRequest.md) |  | 

### Return type

[**MaintenanceClearResponse**](MaintenanceClearResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## PostMaintenanceReconcile

> MaintenanceReconcileResponse PostMaintenanceReconcile(ctx).MaintenanceReconcileRequest(maintenanceReconcileRequest).Execute()

Reconcile orphaned vector records

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	maintenanceReconcileRequest := *openapiclient.NewMaintenanceReconcileRequest() // MaintenanceReconcileRequest |  (optional)

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.PostMaintenanceReconcile(context.Background()).MaintenanceReconcileRequest(maintenanceReconcileRequest).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.PostMaintenanceReconcile``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `PostMaintenanceReconcile`: MaintenanceReconcileResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.PostMaintenanceReconcile`: %v\n", resp)
}
```

### Path Parameters



### Other Parameters

Other parameters are passed through a pointer to a apiPostMaintenanceReconcileRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **maintenanceReconcileRequest** | [**MaintenanceReconcileRequest**](MaintenanceReconcileRequest.md) |  | 

### Return type

[**MaintenanceReconcileResponse**](MaintenanceReconcileResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## PostMaintenanceRepair

> MaintenanceRepairResponse PostMaintenanceRepair(ctx).MaintenanceRepairRequest(maintenanceRepairRequest).Execute()

Repair a project knowledge index

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	maintenanceRepairRequest := *openapiclient.NewMaintenanceRepairRequest("Path_example", "Project_example") // MaintenanceRepairRequest | 

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.PostMaintenanceRepair(context.Background()).MaintenanceRepairRequest(maintenanceRepairRequest).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.PostMaintenanceRepair``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `PostMaintenanceRepair`: MaintenanceRepairResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.PostMaintenanceRepair`: %v\n", resp)
}
```

### Path Parameters



### Other Parameters

Other parameters are passed through a pointer to a apiPostMaintenanceRepairRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **maintenanceRepairRequest** | [**MaintenanceRepairRequest**](MaintenanceRepairRequest.md) |  | 

### Return type

[**MaintenanceRepairResponse**](MaintenanceRepairResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## PostPromote

> PostPromote(ctx, id).Execute()

Promote a release to active

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	id := int64(789) // int64 | 

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	r, err := apiClient.DefaultAPI.PostPromote(context.Background(), id).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.PostPromote``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
}
```

### Path Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**ctx** | **context.Context** | context for authentication, logging, cancellation, deadlines, tracing, etc.
**id** | **int64** |  | 

### Other Parameters

Other parameters are passed through a pointer to a apiPostPromoteRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------


### Return type

 (empty response body)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: Not defined
- **Accept**: Not defined

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## PostReleases

> CreateReleaseResponse PostReleases(ctx).CreateReleaseRequest(createReleaseRequest).Execute()

Create a new corpus release

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	createReleaseRequest := *openapiclient.NewCreateReleaseRequest("Name_example") // CreateReleaseRequest | 

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.PostReleases(context.Background()).CreateReleaseRequest(createReleaseRequest).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.PostReleases``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `PostReleases`: CreateReleaseResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.PostReleases`: %v\n", resp)
}
```

### Path Parameters



### Other Parameters

Other parameters are passed through a pointer to a apiPostReleasesRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **createReleaseRequest** | [**CreateReleaseRequest**](CreateReleaseRequest.md) |  | 

### Return type

[**CreateReleaseResponse**](CreateReleaseResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## PostReviewAccept

> PostReviewAccept(ctx, id).ReviewAcceptRequest(reviewAcceptRequest).Execute()

Accept a staged document

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	id := int64(789) // int64 | 
	reviewAcceptRequest := *openapiclient.NewReviewAcceptRequest() // ReviewAcceptRequest |  (optional)

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	r, err := apiClient.DefaultAPI.PostReviewAccept(context.Background(), id).ReviewAcceptRequest(reviewAcceptRequest).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.PostReviewAccept``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
}
```

### Path Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**ctx** | **context.Context** | context for authentication, logging, cancellation, deadlines, tracing, etc.
**id** | **int64** |  | 

### Other Parameters

Other parameters are passed through a pointer to a apiPostReviewAcceptRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------

 **reviewAcceptRequest** | [**ReviewAcceptRequest**](ReviewAcceptRequest.md) |  | 

### Return type

 (empty response body)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: Not defined

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## PostReviewReject

> PostReviewReject(ctx, id).ReviewRejectRequest(reviewRejectRequest).Execute()

Reject a staged document

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	id := int64(789) // int64 | 
	reviewRejectRequest := *openapiclient.NewReviewRejectRequest("Reason_example") // ReviewRejectRequest | 

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	r, err := apiClient.DefaultAPI.PostReviewReject(context.Background(), id).ReviewRejectRequest(reviewRejectRequest).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.PostReviewReject``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
}
```

### Path Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**ctx** | **context.Context** | context for authentication, logging, cancellation, deadlines, tracing, etc.
**id** | **int64** |  | 

### Other Parameters

Other parameters are passed through a pointer to a apiPostReviewRejectRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------

 **reviewRejectRequest** | [**ReviewRejectRequest**](ReviewRejectRequest.md) |  | 

### Return type

 (empty response body)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: Not defined

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## PostRollback

> PostRollback(ctx, id).RollbackRequest(rollbackRequest).Execute()

Roll back to a prior release

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	id := int64(789) // int64 | 
	rollbackRequest := *openapiclient.NewRollbackRequest() // RollbackRequest |  (optional)

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	r, err := apiClient.DefaultAPI.PostRollback(context.Background(), id).RollbackRequest(rollbackRequest).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.PostRollback``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
}
```

### Path Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**ctx** | **context.Context** | context for authentication, logging, cancellation, deadlines, tracing, etc.
**id** | **int64** |  | 

### Other Parameters

Other parameters are passed through a pointer to a apiPostRollbackRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------

 **rollbackRequest** | [**RollbackRequest**](RollbackRequest.md) |  | 

### Return type

 (empty response body)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: Not defined

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)


## PostSearch

> SearchResponse PostSearch(ctx).SearchRequest(searchRequest).Execute()

Hybrid knowledge search

### Example

```go
package main

import (
	"context"
	"fmt"
	"os"
	openapiclient "github.com/GIT_USER_ID/GIT_REPO_ID/aimeekb"
)

func main() {
	searchRequest := *openapiclient.NewSearchRequest("Query_example") // SearchRequest | 

	configuration := openapiclient.NewConfiguration()
	apiClient := openapiclient.NewAPIClient(configuration)
	resp, r, err := apiClient.DefaultAPI.PostSearch(context.Background()).SearchRequest(searchRequest).Execute()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error when calling `DefaultAPI.PostSearch``: %v\n", err)
		fmt.Fprintf(os.Stderr, "Full HTTP response: %v\n", r)
	}
	// response from `PostSearch`: SearchResponse
	fmt.Fprintf(os.Stdout, "Response from `DefaultAPI.PostSearch`: %v\n", resp)
}
```

### Path Parameters



### Other Parameters

Other parameters are passed through a pointer to a apiPostSearchRequest struct via the builder pattern


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **searchRequest** | [**SearchRequest**](SearchRequest.md) |  | 

### Return type

[**SearchResponse**](SearchResponse.md)

### Authorization

[bearerAuth](../README.md#bearerAuth)

### HTTP request headers

- **Content-Type**: application/json
- **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints)
[[Back to Model list]](../README.md#documentation-for-models)
[[Back to README]](../README.md)

