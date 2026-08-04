module github.com/JBailes/aimee/control-web

go 1.25.0

require (
	github.com/JBailes/aimee/server-go v0.0.0
	github.com/golang-jwt/jwt/v5 v5.3.0
	github.com/mattn/go-sqlite3 v1.14.42
)

replace github.com/JBailes/aimee/server-go => ../server-go
