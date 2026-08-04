module github.com/JBailes/aimee/runtime-web

go 1.25.0

require (
	github.com/JBailes/aimee/server-go v0.0.0
	github.com/RakuenSoftware/smoothgui/auth v0.2.3
	github.com/mattn/go-sqlite3 v1.14.42
)

require github.com/msteinert/pam/v2 v2.1.0 // indirect

replace github.com/JBailes/aimee/server-go => ../server-go
