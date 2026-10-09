// Build with standard `go` (TinyGo's -buildmode=c-shared is wasm-only
// on current releases; see ../README.md "Toolchain note"):
//
//   CGO_ENABLED=1 go build -buildmode=c-shared -o libgreet.so contrib/host/tinygo/examples/greet.go
//
// Then load from Aether via contrib.host.tinygo (see ../README.md).
package main

import "C"

//export Answer
func Answer() int32 { return 42 }

//export Add
func Add(a, b int32) int32 { return a + b }

//export Negate
func Negate(a int32) int32 { return -a }

// Greet returns a C.CString, which cgo allocates with malloc on the C
// heap. Call it through tinygo.call_str_str_owned, which copies the
// result and frees this pointer; the borrowed call_str_str would leak
// it (see the README's "Memory ownership").
//
//export Greet
func Greet(name *C.char) *C.char {
	greeting := "hello, " + C.GoString(name)
	return C.CString(greeting)
}

//export PrintN
func PrintN(n int32) {
	// TinyGo's c-shared programs can println directly; useful for
	// callbacks that side-effect rather than return.
	for i := int32(0); i < n; i++ {
		print(".")
	}
	println()
}

func main() {} // c-shared still requires main()
