package delegate

import "context"

type executionScopeKey struct{}

// Execution scopes are opaque host-issued authorization handles. These helpers
// propagate them; they never derive authority from a principal/model string.
func ValidExecutionScope(scope string) bool {
	if len(scope) != 64 {
		return false
	}
	for _, c := range scope {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return false
		}
	}
	return true
}
func WithExecutionScope(ctx context.Context, scope string) context.Context {
	if ctx == nil {
		ctx = context.Background()
	}
	return context.WithValue(ctx, executionScopeKey{}, scope)
}
func ExecutionScopeFromContext(ctx context.Context) string {
	if ctx == nil {
		return ""
	}
	scope, _ := ctx.Value(executionScopeKey{}).(string)
	return scope
}
