package main

import controlwebpolicy "github.com/JBailes/aimee/server-go/modules/control-web/policy"

// consoleAdminAllows and fleetAllows keep the provider-facing names local while
// sharing the exact authorization policy with the isolated control-web process.
func consoleAdminAllows(method, path string) bool {
	return controlwebpolicy.ConsoleAdminAllows(method, path)
}

func fleetAllows(method, path string) bool {
	return controlwebpolicy.FleetAllows(method, path)
}
