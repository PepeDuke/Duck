# Duck
Duck is a programming language, focusing on speed and abstraction, designed mainly for game development. Duck is a statically typed and compiled language. Main feature of using Duck is async and sync design is resolved by the compiler with full thread safety.
# Philosophy
There are many languages, each great in their own way, the only reason nowadays for making a new language is to do something revolutionary. As CPUs have more and more cores over time there are not many reasons to make single threaded applications, async programming can be ok but from time to time you run into race conditions, concurrency problems or just additional code to stop your threads from fighting over resources. Duck is here to save the day. As many things are inherently async: some I/O operations, code that takes too long to execute, or any network operations. Duck solves for all of this in compile time.

No garbage collector, there should not be generated any garbage in the first place. Duck has a single parent policy. At which the owning parent destroys its children or the parent gets destroyed, the resource gets removed. But until then others can reference this resource allowing for having the same resource with multiple references.

Support for reflection that can be then used while building game engine

Built-in markup tags for building UI/UX

Testing included as core part of the language

Basic data collection types like strings/arrays/maps and so on, are core part of the language. This is firstly that we don't have to create our own or use something like std:: in C++. Secondly this helps Duck create a multithreading specific for collections.

Duck as the name suggests supports duck typing. Why have duck typing in statically typed language? To offer alternative to polymorphism where sometimes we have function that applies to large number of classes/structs but they have nothing else in common, we would be forced to use interfaces or events to append that functionality into our inheritance structure.

Duck forces type safety for functions as they are the worst offenders in automatic return type or automatic arguments. I'm looking at you JavaScript.

# Why this syntax?
Duck syntax is based on my personal preferences and my imagination of the perfect syntax. I like type safety as in my opinion it makes code simpler to read especially as you get return from functions/method and you have to mouse over to see what it is actually returning, with type safety you see it right there. But on the other hand I'm not a fan of verbose syntax, as it makes the whole code unnecessarily too large.

# Line in a sand
We chose to support only 64-bit targets because most modern gaming platforms are 64-bit. This gives pointers a fixed size of 8 bytes across all supported targets.

# Project status
Current status: **early C++ bootstrap compiler**. The language design below is
broader than the implemented subset. See [Compiler/README.md](Compiler/README.md)
for build instructions, executable examples, and current limitations.
The separate [build tool](BuildTool/README.md) builds modular programs from
`.dkp` projects, with explicit dependencies, optional modules, and native linking.
Use `duck build PROJECT --target wasm64` to build a browser application with
8-byte pointers. See the [web build instructions](BuildTool/README.md#building-for-a-browser).
The bootstrap also supports `extern "C"` native API declarations, typed pointers, C-layout structs, native callbacks,
opaque `void*` handles, UTF-16 `string` and UTF-8 `name` values with deterministic cleanup,
and native text arguments; see the
[native interop guide](Compiler/README.md#native-api-calls-and-opaque-handles).

### Compiler-supported targets/platforms

| Capability | Windows x64 | Browser wasm64 |
|---|---|---|
| Parallel execution | Automatic tasks and explicit `P` branches can use workers. | One thread runs the branches. The compiler still checks conflicting accesses and preserves joins. |
| Built-in `file` API | Opens, reads, and writes local files. | The compiler rejects use of the built-in file runtime. |
| TCP and UDP sockets | Connect or listen on TCP, bind UDP, and receive socket callbacks. | Browser pages cannot bind a listening port or open raw TCP/UDP sockets. Duck exposes WebTransport sessions, streams, and datagrams through `Net.ConnectWebTransport`. |
| Platform interop | Link x64 Windows libraries and call Win32 APIs. | External functions need a wasm64 implementation or a JavaScript binding. Windowing and other OS-specific calls require browser equivalents. |
| Running Duck unit tests | `duck test` and `duckc --test` build native test runners. | The Duck test runner is unavailable. Browser integration fixtures build and execute applications in Chromium. |

- [Build and run on Windows x64](Compiler/README.md#build-and-run-on-windows)
- [Build for browser wasm64](BuildTool/README.md#building-for-a-browser)

stages
1. design stage - make sure the syntax is consistent, complete and simple to use
2. C++ bootstrap - write the compiler first in C++ basic lexer/parser, use LLVM
3. write compiler in Duck - use C++ compiler to compile the Duck compiler so the final compiler is written in Duck and does not depend on C++
4. refactor Duck compiler - there will be some functions that are inefficient by the C++ bootstrap and might need to be re-done in assembly.
5. compile optimization - make sure the compiler implements all applicable optimization techniques.
6. feature full compiler - is the syntax feature full, disk operations, memory operations, network operations and graphics.
7. integration for debugging and IDEs - pre-compiler and pdb file emitter might be needed.
8. Something, something advertise Duck - Quack.

# Syntax
*note that this is not final syntax and it is subject to change*

For a guided tutorial with correct patterns and deliberately broken cases, start with [Examples/00_StartHere.dk](Examples/00_StartHere.dk). Each lesson is an independent `.dk` file with explanations in comments. These are design examples; they have not been validated by a Duck compiler. Remaining unspecified edge cases are recorded in [Examples/DesignQuestions.dk](Examples/DesignQuestions.dk).

Duck is **case sensitive**.                  
Duck uses the **.dk** extension for its files.

Example of a Hello world program in Duck:
``` c++
Main()
{
    Print("Hello world")
}
``` 
## Comments in Duck
Comments are the same as in C# or C++ as there is no need to change proven standards.
``` c++
// inline comment
/*
* multi-line comment
*/
```

## Termination in Duck
Separate statements with line breaks. Counted `for` headers use `;` between their three clauses.

## Formatting in Duck
Formatting is up to the user and the formatting in samples should not be taken as the law, if you prefer different formatting you can use it as long as each expression is ended with new line or the } symbol

## Class definitions in Duck
Duck has a struct/class combined into one type called container.
``` c++
Example // <-- this is declaration of container
{ 

}
// or
Example { 

}
```


Functions/methods in Duck. In Duck we call them functions. The difference from containers is they must use () argument space to be declared. () can't be omitted as it is a deterministic factor for functions.
``` c++
Foo()  // <-- this is declaration of function
{

}
// or
Foo () 
{

}
// or
Foo() {

}
```

If a function does not return anything we omit the void type. But if it does return something it needs to specify the type.
``` c++
int Foo() // <-- this is declaration of function with return type
{ 
 return 5
}
```

So in combination with container we might have something like 
``` c++
Example 
{ 
    int Foo() 
    { 
        return 5
    }
}
```
function argument and return have to be type safe
``` c++
int Foo(int i) // <-- argument int i
{
    return i * 2
} 
```
Duck does not support automatic types for return or arguments to preserve information for the programmer.
## Access protection in Duck
Duck supports public and protected access for container members.

The default protection access is **public**.

Setting Access protection in Duck. We use a non-verbose way to specify what the protection is, as it is not most of the time the focus of programmers but more used for the IDE that shows what this object can access. so we use + - operators inspired by UML models as it seems to be the most straightforward way to say what protection is in use.
``` c++
-InternalFoo() // <--  protected function
{ 

}
+Foo() // <-- public function
{

} 
```
Protected members remain accessible to derived containers, allowing them to work with the inherited internal state.

If function has no protection access specified it will be taken as public
``` c++
-InternalFoo() // <--  protected function
{ 

}
Foo() // <-- public function
{

} 
```
the protection access symbol can't have spaces
``` c++
-InternalFoo() // <--  protected function
{ 

}
- InternalFoo2() // <-- Error not valid
{

} 
```

## Function calling in Duck
Duck is object-oriented language and so we can use object calls.
``` c++
// declaration
Example 
{  
    Foo()
    {
    
    }
}

Example ex = Example()  // <-- create variable ex that is type Example
ex.Foo()               // <-- calling of function Foo defined in the declaration inside Example
```

We can also shorten the variable declaration for object by using
``` c++
Example ex()           // <-- create variable ex that is type Example, expression equivalent to Example ex = Example()
```

## Program start in Duck
Each program starts from the Main function that has to be declared **outside of any container**.
``` c++
Main(string [] args) 
{
    Print("Hello world")
}
``` 
*or*
``` c++
Main() 
{
    Print("Hello world")
}
``` 

`args` holds the program's arguments after its own name, in order, and is empty when there are none.

we can execute instructions just placed outside of containers inside file but we need to call the Exec function on the file as they do not run by default.
``` c++
// inside main.dk
Main(string [] args) {
    Exec("hello.dk")  // <- Path to file
}
// inside hello.dk
    Print("Hello world")
``` 

The path is a string literal, relative to the file that calls `Exec`, and that file must be compiled with the program. A path that matches no source file is a compile error. Each `Exec` runs the statements again, and variables declared by them last for that one run.

## Static calls in Duck
We have chosen # to represent static meanings depending on context. we can define static Container by # before its body
``` c++
Example 
#{
   // everything in here is static
}
```
and we can call static function by
``` c++
Example 
#{
   Foo() // <-- static function
   {
   
   }
}

Example.Foo() // <-- static function call
```

## Container blocks in Duck

we can also chain static and non-static bodies of container to get object definition and static functions with the same name. This separation will hopefully make logical blocks, that programmers can document and collapse as needed.
``` c++
Example 
#{
  // static part, everything in here is static
}
{
  // object part, everything in here is bound to object
}
```

we can chain as many as we want together
``` c++
Example 
#{}
{}
#{}
#{}
{} 
// still valid code
```

container blocks can't be separated by space
``` c++
Example 
#{}

{}// error invalid container definition
```

if multiple containers have the same name and parent container they are considered the same container. 
``` c++
Example // <--  non-static container
{  
    Foo() // <-- not accessible by  Example.Foo() 
    {

    }
}

Example  // <--  only-static container any variables or functions in here are static
#{ // <-- static indicator #
    Foo() // <-- accessible by Example.Foo() 
    {

    }
}
```
So above and below examples are equivalent
``` c++
Example // <--  non-static  container
{  
    Foo() // <-- not accessible by Example.Foo() 
    {

    }
}
#{    // <--  only-static container any variables or functions in here are static
    Foo() // <-- accessible by Example.Foo() 
    {

    }
}
```

## Variables in Duck
all variable types are lower case, no uppercase types exist, if in Duck you want to get type Max you do int.Max 
List of allowed types:
``` c++
void
bool
byte
short
int
long
float
double
name     // UTF-8 text (8-bit code units)
string   // UTF-16 text on every platform (16-bit code units)
char     // one Unicode scalar value, e.g. '🦆' (not a code unit)
uint
uint8
uint16
uint32
uint64
int8
int16
int32
int64
u1-64   // example u2, 2 byte unsigned integer (u1/u2/u4/u8 = uint8..uint64)
i1-64   // example i8, 8 byte signed integer (i8 = long; i64 holds 512 bits)
date     // civil date, proleptic Gregorian, 0001-01-01 .. 9999-12-31
time     // time of day, 00:00:00 .. 23:59:59.999999999 (nanoseconds)
datetime // date + time; none of the three carry a timezone
vec2 vec3 vec4    // 2, 3 or 4 floats
dvec2 dvec3 dvec4 // 2, 3 or 4 doubles
ivec2 ivec3 ivec4 // 2, 3 or 4 ints
mat3 mat4         // 3x3 or 4x4 floats, column-major
dmat3 dmat4       // 3x3 or 4x4 doubles, column-major
quat              // rotation as a quaternion: X, Y, Z, W floats
rot               // rotation as Euler angles in radians: Pitch, Yaw, Roll floats
// special types are
big   // <-- that holds arbitrary size of int number --- should be used only for working with very large numbers
ubig  // nonnegative big; a subtraction that would go below zero traps
tiny  // <-- holds floating point number of 69 digits  --- should be used only for working with very small numbers to get precision
utiny // nonnegative tiny; results are rounded to 69 significant digits, ties to even 
element // <-- single element of a tags tree
markup // <-- hold tags tree for Mark Up  
func // <-- holds function
macro // <-- return type of macros
```

`name` is UTF-8 and `string` is UTF-16 on every platform. These encodings do not
follow the host's `wchar_t` or `TCHAR` configuration. A code unit is not
necessarily one complete Unicode character. Native API adapters own any
required encoding conversions. The bootstrap compiler's explicit `cstring`
is a separate native interop type, not an implementation of either language type.

`char` holds one Unicode scalar, so a supplementary character is one `char`
even though it takes two `string` code units. `date`, `time`, and `datetime`
are wall-clock values with no timezone. All four are built with validated
constructors such as `char(65)`, `date(2024, 2, 29)`, and `time(12, 30, 0)`;
an invalid value such as `date(2023, 2, 29)` is rejected. See the
[compiler notes](Compiler/README.md#characters-dates-and-times) for details.

Integer literals may use decimal, hexadecimal (`0xFF`), or binary (`0b1010`)
notation. Prefixes may also be uppercase. Their values use the same type
inference and range checks regardless of base; `0xFF` means positive 255.
Leading zeroes alone keep decimal notation (`012` means 12).

### Comparing floats

`==` and `!=` on `float` and `double` allow for rounding error, so
`0.1 + 0.2 == 0.3` is true. Two values match when they differ by at most
`0.00001` for float or `0.000000001` for double, scaled up by the larger value
once it is above 1. Infinities equal themselves and NaN equals nothing. `<`,
`>`, `<=` and `>=` stay exact.

To set the tolerance yourself, put it in parentheses directly before the
operator, with no space between `)` and `==`. It can be any number expression:

``` c++
if a (0.5)== b { }       // true when a and b differ by at most 0.5
if a (0.5)!= b { }       // true when they differ by more
if a (0.0)== b { }       // exact comparison
if a (step * 2.0)== b { }
```

There must be a space before the `(`, because `f(0.5)== b` calls `f`.

### Vectors and matrices

The vector and matrix types are built into the compiler. They are value types:
assigning or passing one copies it. Components are `X`, `Y`, `Z` and `W`, or an
index. A matrix is indexed by column, so `m[column][row]`. A declared but
unassigned vector or matrix is all zeros.

``` c++
vec3 position = vec3(1.0, 2.0, 3.0)
position.Y += 1.0
Print(position[1])            // 3
Print(position)               // (1, 3, 3)

mat4 transform = mat4(1.0)    // identity
transform[3][0] = 5.0         // column 3, row 0
Print(transform * vec4(1.0))  // (6, 1, 1, 1)
```

| Constructor | Result |
| --- | --- |
| `vec3()` | all zeros |
| `vec3(x, y, z)` | the given components |
| `vec3(s)` | every component set to `s` |
| `vec4(v2, z, w)` | scalars and vectors in order, adding up to the size |
| `vec3(v4)` | the leading components of a larger vector |
| `dvec3(v)`, `ivec3(v)`, `vec3(d)` | a vector of another scalar type |
| `mat3(s)` | `s` on the diagonal, zeros elsewhere |
| `mat3(c0, c1, c2)` | the given column vectors |
| `mat3(a, b, c, d, e, f, g, h, i)` | scalars in column order |
| `mat3(m4)`, `mat4(m3)`, `dmat4(m)` | the overlap, with identity filling the rest |

The operators are:

| Expression | Meaning |
| --- | --- |
| `a + b`, `a - b`, `a * b`, `a / b` on two vectors | component by component |
| vector with a scalar, on either side | applied to each component |
| `m + n`, `m - n` | component by component |
| `m * n`, `m * v` | matrix product, and matrix times column vector |
| `m * s`, `s * m`, `m / s` | applied to each component |
| `-a` | negates each component |
| `a == b`, `a != b` | `==` is true only when every component matches, with the float tolerance above |
| `a (0.5)== b` | every component within the given tolerance |
| `+=`, `-=`, `*=`, `/=` | as above, when the result keeps the left side's type |

Both operands must use the same scalar type; `vec3 + dvec3` is an error, so
convert one with `dvec3(v)`. A lone scalar follows the usual implicit
conversions, so `dvec3 * 2.0` works and `vec3 * someDouble` does not. An index
outside the vector or matrix traps, as does dividing an `ivec` by zero.

Vectors and matrices also have methods. `Normalize` changes the vector it is
called on and also returns it. Every other method returns a new value and
leaves the original unchanged, so ignoring its result is a compile error.
`Math.Normalize(v)` returns a new vector and leaves `v` alone.

``` c++
target.Normalize()                      // target is now unit length
vec3 direction = target.Normalize()     // also gives you the result
float speed = velocity.Length()
vec3 side = forward.Cross(up)
mat4 view = camera.Inverse()
```

| Called on | Methods |
| --- | --- |
| `vecN`, `dvecN` | `Length()` `LengthSquared()` `Normalize()` `Dot(v)` `Distance(v)` `Lerp(v, t)` |
| `vec3`, `dvec3` | `Cross(v)` |
| every vector, including `ivecN` | `Abs()` `Min(v)` `Max(v)` |
| `matN`, `dmatN` | `Transpose()` `Inverse()` `Determinant()` |

`Normalize` leaves a zero vector at zero. `Min` and `Max` work component by
component. `Inverse` of a singular matrix gives infinities or NaN, so check
`Determinant()` first when a matrix might not be invertible.

The types themselves provide starting values:

| Function | Result |
| --- | --- |
| `mat4.Identity()` | ones on the diagonal, zeros elsewhere |
| `vec3.Identity()`, `vec3.One()` | all ones, so `v * vec3.Identity() == v` |
| `vec3.Zero()`, `mat4.Zero()` | all zeros |

Vectors work directly with `Math.Dot`, `Math.Cross`, `Math.Length`,
`Math.Distance` and `Math.Normalize`, except `ivec`, which has no fractional
result to give.

### Rotations

`quat` holds a rotation as a quaternion and `rot` holds one as Euler angles in
radians. `Pitch` turns about X, `Yaw` about Y and `Roll` about Z. Roll applies
first, then pitch, then yaw. Rotations are right-handed.

``` c++
quat q = quat.FromEuler(0.0, 1.57, 0.0)       // pitch, yaw, roll
vec3 turned = q.Rotate(vec3(0.0, 0.0, 1.0))   // about (1, 0, 0)

rot aim = rot(0.1, 0.5, 0.0)
q.FromEuler(aim)                              // q now holds aim's rotation
quat both = q * other                         // other applies first
mat4 transform = both.ToMat4()
```

| Function | Result |
| --- | --- |
| `quat.Identity()` | no rotation, `(0, 0, 0, 1)` |
| `quat.FromEuler(rot)`, `quat.FromEuler(pitch, yaw, roll)` | a quaternion from Euler angles |
| `quat.FromAxisAngle(axis, angle)` | a turn about a `vec3` axis, which need not be unit length |
| `rot.Identity()`, `rot.Zero()` | no rotation |

| Method | Result |
| --- | --- |
| `q.Rotate(v)`, `r.Rotate(v)` | `v` rotated, as a new `vec3` |
| `q.FromEuler(...)`, `q.FromAxisAngle(axis, angle)` | sets `q`, and also returns it |
| `q.Normalize()` | makes `q` unit length, and also returns it |
| `q.Length()`, `q.LengthSquared()`, `q.Dot(p)` | as for vectors |
| `q.Conjugate()`, `q.Inverse()` | the reverse rotation; `Inverse` also works when `q` is not unit length |
| `q.Slerp(p, t)` | the rotation `t` of the way to `p`, taking the shorter way round |
| `q.ToEuler()` | a `rot` |
| `r.ToQuat()` | a `quat` |
| `q.ToMat3()`, `q.ToMat4()`, `r.ToMat3()`, `r.ToMat4()` | the rotation matrix |

`q * p` composes two rotations with `p` applying first, and `q * v` rotates a
`vec3`. `==`, `!=` and a written tolerance work as for vectors, but `q` and `-q`
are the same rotation and still compare unequal. A `rot` also has `+`, `-` and
scaling by a number, and `q.Rotate`, `q * v` and the matrix conversions assume a
unit-length `q`.

A declared but unassigned `quat` is no rotation, `(0, 0, 0, 1)`, and so is
`quat()`. At pitch of straight up or down, yaw and roll turn
the same axis, so `ToEuler` reports all of that turn as yaw.

### Memory layout

Components are tightly packed, so `vec3` is 12 bytes and `mat4` is 64 bytes
laid out as sixteen floats. That matches GLSL `mat4` and vertex attributes. It
does not match `vec3` or `mat3` inside a `std140` or `std430` block, where
GLSL pads each three-component column to 16 bytes.

## Explicit type casts in Duck

Duck uses C-style cast syntax: put the target type in parentheses before the
expression to convert.

``` c++
int count = 7
long wide = (long)count
double precise = (double)count
int64 sameWidth = (int64)wide
```

A cast binds like a unary operator. `(long)a + b` casts only `a`, while
`(long)(a + b)` casts the result of the addition. Casts can be nested, such as
`(double)(long)count`. Parentheses without a type still group expressions.

Explicit casts do not change the declared type of the source variable and do
not produce a mutable reference. A function expecting `long&` requires a real
`long` variable; `(long)value` is a converted value, not a reference argument.

The current numeric cast rules are:

- Floating-point to integer floors toward negative infinity: `(int)9.99` is `9`
  and `(int)-9.99` is `-10`. NaN, infinity, or a floored result outside the target
  integer range causes a runtime trap. This differs from C/C++ despite using
  the same cast syntax.
- Narrowing integer casts retain the destination's low bits. Equal-width
  signed/unsigned casts reinterpret the bits. Widening sign-extends signed
  sources and zero-extends unsigned sources.
- Floating-point narrowing may round or overflow to infinity. A cast of an
  already-rounded value does not recover precision.
- Numeric-to-`bool` is false for zero and true otherwise (including NaN).
  `bool` converts to numeric `0` or `1`.

The bootstrap supports casts among `int8`, `short`, `int`, `long`, `uint8`,
`uint16`, `uint32`, `uint64`, `float`, `double`, and `bool`, including
`int16`/`int32`/`int64`, `byte` (`uint8`), and `uint` (`uint32`) aliases.
Floating-to-unsigned casts also floor and check the target's range; negative
floored values trap. See [compiler numeric rules](Compiler/README.md) for
literal inference and mixed signed/unsigned conversions.
Explicit casts between `string` (UTF-16) and `name` (UTF-8) transcode text.
Casts involving objects, pointers, `cstring`, and `void` are not implemented. Implicit narrowing remains rejected; write an
explicit cast when conversion is intended.

## Constant variables in Duck
as cont sign we use the same as static
``` c++
#int age = 5  // <- this is constant value, so it can't be changed
age = 6       // <- error: cannot modify constant 'age'
#int limit    // <- error: a constant requires an initializer
```

## Functions in Duck
void type can be omitted or specified
``` c++
Example
{ 
    Foo()
    {

    }

    void Foo2() // same as Foo
    {

    }
}

```

A reference parameter is declared with the & symbol, as in C++.
``` c++
Example
{ 
    int AddCount(int& Count)
    {
        Count++
        return Count;
    }
}

```

## Automatic type declaration in Duck
if you want to let the compiler decide what the type is. I have found that var, let, auto or whatever you call this type is only used to tell the compiler that in fact is his job to decide, instead we choose non-verbose way to tell this compiler with : sign.
``` c++ 
age : 54  // <- this will be compiled as int age = 54 
```
## Null references in Duck
Setting the owning reference to null requests destruction of the resource and invalidates all its aliases. Destruction is deferred: if a function is still using the resource, Duck waits for that use to finish before completing cleanup.
``` c++
Example* ex() 
ex.Foo()      // <--valid call
ex = null
ex.Foo()      // <-- Error, ex container no longer exists
```

we can check if variable is null by
``` c++
if ex
  ex.Foo()
// or
?ex.Foo()   // <-- shorter version of if not null above. will be only executed if ex is valid

// ? operator checks all containers in call so it can be something like
Owner
{
  Example* ex() 
}

Owner* o()
?o.ex.Foo()  // <-- in this case it checks not null for ex and for o containers
```

Both `if ex` followed by `ex.Foo()` and `?ex.Foo()` protect the checked use against null invalidation during the call. The `?ex.Foo()` form executes only if the reference and the function are valid; otherwise it skips the call.

A pointer works in any condition, and is true when it is not null: `while ex`, `!ex`, `ex && other`, and `ex ? a | b`.

`?o.ex.Foo(x)` checks each reference on the way, once and from left to right. At the first invalid one it skips the rest, so `x` is not evaluated. Each live reference stays valid until the statement ends. A skipped call that returns a value gives that type's default, such as `0`, `""`, or `null`. A missing collection entry, as in `?items[5].Foo()`, or an empty `func` field also skips the call.

Inside `if ex`, the name `ex` keeps referring to the object the condition found, even if its owner lets go of it during the branch. The object is cleaned up when the branch ends, whether it falls through, returns, breaks, or continues. New checks such as `ex == null` already see it as gone. A branch that itself reassigns or moves `ex` gets an ordinary null test instead.
## Heap allocation in Duck
when variable gets initialized we also allocate the variable. we use C++ style pointer symbol * to specify if it's stack allocation or heap allocated
``` c++ 
Example
{
    int Val

    Foo()
    {
    }
} 

// Stack allocation
Example e = Example()
Example e2()
e3 : Example()

// Heap allocation
Example* e = Example()
Example* e2()
e3 :: Example()

// Stack allocation
int num = 5
num2 : 5

// Heap allocation
int* num = 5
num2 :: 5

// Stack allocation
Example e3 = e2
Example e4 = {Val : 5}

// Heap allocation
Example* e3 = e2
Example* e4 = {Val : 5}

// Depends on the allocation of e2, if it can't be determined it will prefer stack
e3 : e2

// Heap allocation, force heap allocation
e3 :: e2 

// not allocated, technically pointer is on stack but there is no heap allocation
Example* e
Example* e2 = null
```

Use `.` to access a field through either a value or a pointer.

```c++
Example local()
Example* onHeap()
local.Val = 3
onHeap.Val = local.Val
```

If we want to do in-place heap allocation we can use the :: symbol
``` c++
Example* [] examples
examples += ::Example()
```

`void*` is an untracked native handle. The API that provides it determines whether and how to release it.

## Object references in Duck
In Duck there is only one owner of the value called parent, but this value can still have more references.
``` c++ 
Example
{} // declaration

Example* e = Example()
e2 : e  // <-- holds now reference to e value but it is not its parent
e = null 
Print(e2 == null)       // <-- e2 prints out true as the referenced value is now null 
```

Ownership can be transferred with `::`. The previous owning reference becomes null. Existing aliases continue to reference the same resource under its new owner.
``` c++ 
Example* e = Example()
alias : e   // non-owning reference
e2 :: e     // e2 now owns the resource; e becomes null
Print(e == null)    // true
Print(alias == null) // false; alias follows the resource owned by e2
e = e2      // explicitly make e a non-owning alias again
e = null    // clearing an alias does not destroy the resource
Print(e2 == null)   // <-- e2 prints out false as it is still valid
```
Ownership transfer applies only to heap-allocated resources and must use the current owning reference as its source. Taking ownership through a non-owning alias is invalid.

``` c++
Example* owner()
alias : owner
newOwner :: alias // Error: alias is not the owning reference
```

When a resource is destroyed, every alias becomes null, regardless of whether it is stored in a local variable, a container field, a collection, or a captured lambda. Collection entries are retained with null values; they are not removed.

``` c++
Example* owner()
aliases : {owner}
owner = null
Print(aliases[0] == null) // true; the collection still has one entry
```

The new owner's scope determines the resource's lifetime, even when that scope is shorter than the original owner's scope.

``` c++
Example* owner()
alias : owner
if true
{
    temporary :: owner // owner becomes null
} // temporary's scope ends: destruction is requested and alias becomes null
?owner.Foo() // skipped: owner is null
?alias.Foo() // skipped: alias is null
```


Factory functions can return stack-allocated values normally. Returning ownership of a heap-allocated resource requires explicit `return ::resource`; a plain return of a heap reference does not transfer ownership.
``` c++ 
Cake
{
   int Slices

   Eat()
   {
     if Slices <= 0
        Print("cake has been eaten")
     else
        Slices--
   }
}

CakeFactory
#{
       Cake MakeCake()
       {
           Cake c() // Stack allocation
           c.Slices = 2
           return c
       }
}

cake : CakeFactory.MakeCake()
cake.Eat()
cake.Eat()
cake.Eat()  // cake has been eaten
```

when heap allocated
``` c++ 
Cake
{
   int Slices

   Eat()
   {
     if Slices <= 0
        Print("cake has been eaten")
     else
        Slices--
   }
}

CakeFactory
#{
       Cake* MakeCake(bool retMain)
       {
           Cake* c = Cake() // Heap allocation
           c.Slices = 2
           if retMain
             return ::c // returns ownership
           else
             return c  // return reference
       }
}

cake : CakeFactory.MakeCake(true)
cake.Eat()
cake.Eat()
cake.Eat()  // cake has been eaten

//
cake2 : CakeFactory.MakeCake(false) // returns null as the reference to c in the MakeCake function gets freed
cake2.Eat() // <-- Error null reference

```

A function can return ownership on one path and a tracked reference on another. The returned value carries which one it is, and the way you receive it decides what happens:

| Receiver | Owning result | Borrowed result |
| --- | --- | --- |
| `x : Make()` | `x` becomes the owner | `x` is an alias |
| `x :: Make()` | `x` becomes the owner | runtime error, or a compile error if `Make` never gives ownership |

So `:` accepts anything a call returns, and `::` is a demand for ownership. The same demand applies to `node.Next :: Make()`, where `(node.Next :: Make())` gives `false` instead of an error. A local received with `:` can be cleared like any other, and only destroys the object if it really owned it. Moving from it with `::` needs real ownership too.

A heap reference result has to be stored before use, so `x : Find()` then `x.Foo()` rather than `Find().Foo()`.


A heap object has one owner and any number of non-owning aliases. Copying a
reference creates an alias. Clearing an alias releases only that binding.
Clearing the owner or leaving its scope requests destruction.

```c++
Item* first()

alias : first
next :: first          // first becomes null

alias.Value = 7
Print(next.Value)      // 7

next = null
Print(alias == null)   // true
```

Only the current owner may transfer ownership. Moving an owner preserves the
object's identity and all its aliases. Moving it into a shorter scope makes
that scope responsible for destruction.

A container field can own a heap object, refer to one, or hold a raw pointer.
The declaration says which:

```c++
Node
{
    Item* Part()         // owning, built with the Node
    Node* Next :: null   // owning, starts empty
    Node* Parent         // tracked alias: never keeps the object alive
    void* Handle         // raw, as are pointers to scalars such as int*
}
```

A container with an owning field is move-only and destroys its children with
itself. A move into a field is written `node.Next :: other`, or `this.Next ::
other` inside a method, since a bare `Next :: other` declares a local. The
source is an owner, a call that returns ownership, or a new value, as in
`node.Next :: Node()`. `taken :: node.Next` moves an object back out.

Owning fields form a tree. Moving an object into one of its own descendants
would create an ownership cycle, so the move is rejected before anything
changes, and the original owner stays intact. When the path shows the cycle,
as in `node.Next :: node`, it is a compile error. Otherwise the statement
traps, and the parenthesized form `(below.Next :: top)` gives `false` instead.
Non-owning references can point back to ancestors.

Factories transfer ownership explicitly with `return ::item`. A plain return
of a tracked reference lends it: it stays usable while its owner survives, and
one whose owner was a local of the function arrives expired, because the owner
is cleaned up as the function returns.

```c++
Item* MakeItem()
{
    Item* item()
    return ::item
}

item :: MakeItem()
```

The [open design questions](Examples/DesignQuestions.dk) cover assignment into
an existing owner and owning collection slots.

### Lazy reference tracking

A heap owner starts as one word pointing directly to the object. Ownership moves
and proven short borrows use that representation. When execution first creates
an alias that could outlive the owner, Duck switches the owner to a control
block. Subsequent aliases share it. On x64 the block holds a payload pointer, a
reference count, an active-use count, and the cleanup function in 32 bytes,
excluding allocator overhead.

Releasing the owner closes the object in one atomic step: no new use can begin,
and all aliases compare equal to null. Calls already using the object finish
first, and whichever of them ends last runs the cleanup, exactly once. The last
reference releases the block. Aliases live in locals, container fields,
collection entries, and captured lambdas alike. Sharing heap objects between
threads is future work.

##  Pointers in Duck
Heap allocations are inherently pointer and do have size of 8 bytes

``` c++ 
Example
{}

Example* e = Example() // pointer to allocated Example
```

An empty pointer can be declared without calling the constructor.
``` c++ 
Example
{}

Example* e = null // pointer to Example type with the value of null
Print(e == null) // prints true
```

Any move operation done to the pointers is interpreted as unsafe and the pointer will no longer be tracked by the main owner policy.
``` c++ 
Kid
{
    Foo()
    {

    }
}

Family
{
    Kid* Bob
    Kid* Rob

    Family()
    {
        Bob() // calling constructor on null pointer will allocate it
        Rob()
    }
}

Family* e = Family()

(e.Bob + 1).Foo() // "safe" <-- Foo from rob
e.Bob += 1 // unsafe <-- will actually create a memory leak
e.Rob = null

Print(e.Bob == null) // prints false, as it is no longer tracked it will not get set to null

```



The duck's native pointers carry machine addresses. For example,
`State* p = &state` borrows the address of existing storage, and
`p.Value` accesses its field. Pointer arithmetic is unsupported.

In the tracking design, a Windows callback resolves a Duck handle saved in
`GWLP_USERDATA` to live state or `null`. Calls such as `GetClientRect(window, &bounds)`
receive a native address that stays valid for the call.

##  Constructors/Destructors in Duck
In Game environment you usually have functions like Start/Spawn, Tick/Update, Destroy or variants on this, that are actually separated from Constructors and Destructors. this relates to the life cycle of this object, where destructor is final and you want to destroy it only at the end of the frame, for start/Spawn you might want to do something before the constructor is called

``` c++ 
Example
{
    Example() // <-- basic constructor
    {

    }

    ~Example() // <-- basic destructor
    {

    }
}

```

Duck offers option to deal with Constructor issue, using partial initialization:
``` c++ 
Example
{
    -Example() // <-- partial constructor that gets called before the regular one, but it can be set after, this constructor gets called regardless of what is called in other constructors and in children
    {
        Print("Hello")
    }

    Example() // <-- basic constructor
    {
        Print("World")
    }
}
Example e() // <-- Prints 
// Hello
// World

```

Common issue is where we want to assign world to object that lives there
``` c++ 
Entity
{
    -Entity(World world) // <-- partial constructor
    {
        Print("Hello")
    }
}

Gun : Entity
{
    Gun(int ammo) // <-- basic constructor
    {
        Print("World")
    }

    foo()
    {

    }
}
World w()
Gun g(5)
g.foo() // <-- error incomplete initialization
g(w)// <-- Prints 
// Hello
// World

// or
Gun g(w,5) // <-- arguments create new composite constructor
// <-- Prints 
// Hello
// World

```

Duck also lets you call the constructor again to reinitialize if needed
``` c++ 
Example
{
    Example() // <-- basic constructor
    {
        Print("Hello World")
    }

    foo()
    {
        Print("Foo")
    }
}

Example e() // <-- Prints Hello World
g.foo() // <-- Prints Foo
e() // <-- Prints Hello World


```

##  Collections in Duck
collections may be statically allocated on stack or dynamic allocated on heap.

``` c++ 
int [50] s1  // <-- static array that holds 50 int values
int [] s2    // <-- dynamic array can hold any number of elements, might be same as (in C# List, in C++ std::vector)
```
Other collection are separate as they are functionally different.
``` c++
int queue s1            // queue of integers  - FIFO
int stack s2            // stack of integers  - LIFO
string, string pair s3  // <-- in C++ map,  in C# called dictionary, and honestly the best name would be KeyPair, but pair must do as it is shorter and clearly says what it does
```
we can assign values by = and use { } braces to insert actual values.
``` c++
int []  s1 = {5,1,2} 
int queue s2 = {2,3,4}   // <- 2 is first and 4 last in queue so if we dequeue we will get 2
int stack s3 = {2,3,4}   // <- 2 is first and 4 last in stack so if we pop we will get 4
string, string pair s4 = {"name" : "Bob", "address" : "Code square 5", "city": "New York"}
```
pair has also case where you add only one type 
``` c++
string pair s1 = {name: "Bob", address : "Code square 5"} 
Print(s1[name])  // <-- prints Bob
```
this is similar to "string, string pair" in use cases but it is marginally faster.

## Automatic type collections in Duck
as with variables we use the : symbol
``` c++
s1 : {5,1,2}  // will be interpreted as int []  s1 = {5,1,2} 
s2 : {"name" : "Bob", "address" : "Code square 5"} // will be interpreted as pair
```
queue and stack has no automatic type declaration.
## Working with collections in Duck
in Duck we use non-verbose way to work with collections
``` c++
s += 5     // adds value 5 to array
s++        // adds value 0 to array
s[1] += 5  // adds 5 to a value in array with index 1
s[0] = 5   // changes value at index 0 to be 5
s -= [1]   // remove value at index 1, this shifts all values after index 0 by index-1 positions in array
s--        // removes last value in array 
s.Len      // number of elements in array
```
working with pair collection
``` c++
string, string pair s 
s += "name" : "Bob"  // add pair Key: "name", Value: "Bob" 
s["name"] = "Tom"    // change Value of Key "name" to "Tom" 
s++                  // tries to pattern match key (similar to MS Excel for example month) if it fails add 1 to last key and sets value to none
s--                  // removes at last added key
s -= ["name"]        // remove pair Key: "name", Value: "Tom"
s.Clear()            // removes all elements in collection
```
if we need to know the index of add or remove we can add your assignment into () brackets then this expression will return an appropriate return type to the expression
``` c++
int [] s = {1,1}
index : (s += 4)  // the index of the inserted value, a long
Print(index)      // prints 2
last : (s--)      // removing gives the removed value
Print(last)       // prints 4
```

## Bounds checking in Duck
if you need to check if value is valid you can use IsValid() function or use shorten if check
``` c++
int [5] s 
if IsValid(s[6]) // this checks bound of the array s
  s[6] = 5       // this will not be executed as 6 is out of bounds
//or
?s[6] = 5        // ? checks bound of the array s and will not be executed as 6 is out of bounds
x : ?s[6]        // a guarded read gives the default value of the element type when out of bounds, here 0
```
## Multidimensional collections in Duck
Duck supports multidimensional collections for multiple use cases 
``` c++
int [5][10] s1  // <-- 2D static array: 5 arrays of 10, s1[4][9] is the last value
int [][] s2     // <-- 2D dynamic array: s2++ adds a row, s2[0] += 5 adds to row 0
int [-][-] s3   // <-- 2D sparse grid that has index from int.Min to int.Max; unwritten cells read as 0
                //     Meaning you can access values that are in negative index such as s3[-2][1] this is great for grids as you generally start at 0,0
```

## Collection size prediction in Duck
for Duck compiler to accurately calculate execution time for async separation, as programmers we can help with that by adding our guesstimate into array declaration
``` c++
int [] s       // <-- dynamic array can hold any number of elements, compiler makes the guess how many elements might contain depending on its uses in code
int [*] s      // <-- still dynamic array, but this tells the compiler to expect a huge amount of values in this array.
int [50*] s    // <--still dynamic array, but this says to the compiler, expect around 50 values +- some values in this array.
```

we might want to have heap allocated array, aka array of pointers
``` c++
int* [] s
```

## If statement in Duck
if else statements work as in any other language.
``` c++
if i < 5 // <-- if statement with multi line body 
{ 
  i++ 
}
else
{
 i-- 
}


if i > 10 // <-- if statement with single line body
   i--  
else
   i++ 
   
 i = (i == 5)?  0 | 1  // ternary if statement
```
Even though : is convention in ternary conditional operators, symbol | seems more logical to use as in programming || means logical OR and single | can mean pipe or disjunction, it makes more sense as representation of if-else split.
## Loops in Duck
Duck uses for loop as a for and foreach, similarly to JavaScript
``` c++
int [10] loops 
for int i = 0;  i < loops.Len;  i++  // <-- basic for loop
{ 
 // loop body that in this case would be executed 10 times
  loops[i].Foo()
}
for i : 0; i < loops.Len; i++  // interpreted as loop above but we use automatic type declaration
{ 
  loops[i].Foo()
}

for loop : loops  // <-- for each loop in loops
{     
  loop.foo()
}
for -loop : loops {      // <-- reverse for each loop in loops  -- preferred if removing elements
  if loop.PendingDestroy // if some variable is true
   loops -= loop        // <-- removes this element
}
```
Duck omits type as for each value must be the same type, Duck allows for sequence declaration with ... operator as it is the most used way to use loops
``` c++
for i : 0...5 // <-- from 0 to 5 inclusively, it executes loop body 6 times in total
{ 
}
for i : 5...0 // <-- from 5 to 0 inclusively
{
}
```
sequence declaration with ... operator can be used outside of loops
``` c++
n : 0...5  // this will be interpreted as int [] n = {0,1,2,3,4,5}
```
in ... operator we can also use variables to achieve the standard loop pattern "int i = 0; i < loops.Len;  i++"

``` c++
for i : 0...loops.Len-1 
{
}
```
do-while and while work as in other languages so does keywords such as break continue
``` c++
while true // while with body after condition
{ 
  Work()
}

do 
{
  Work()
} while true; // while with body before condition

do 
{
 if IsWorkDone() == true
    break
    
  Work()
}

```

## Loop chaining in Duck 
Duck introducing non-verbose way to have nested loops, with the | symbol used as loop separator 
``` c++
for n : 0...3 |
    i : 0...5 
{
    Print(n)
} // <-- these are two nested for loops, the inner for loop has body of {print(n)}
// Result would be 000000111111222222333333 

// verbose way
for n : 0; n <= 3; n++ 
{ 
  for i : 0; i <= 5; i++ 
  { 
    Print(n)
  }
}
```
we can also add body to the outer loop
``` c++
for n : 0...3 |
{ // <-- outer loop body is executed after inner loop due to | separator being in front of the body
    n++
} 
    i : 0...5 
{
    Print(n)
} 
// Result would be 000000222222  

for n : 0...3 
{
    n++
}| // <-- outer loop body is executed before inner loop due to | separator being behind the body
    i : 0...5 
{
    Print(n)
} 
// Result would be 111111333333   
```
this works only with for loops as I have found you generally want to do nested loops for 2D or 3D arrays or objects within objects.

## Namespaces in Duck 
Duck has no namespace keyword as it is not needed we can simply use #{} static container as a namespace as it serves the same purpose
``` c++
Name // we create our namespace by creating static container
#{ 
   Space 
   #{

         Foo(){}
    }
    {
         Foo2(){}
    }       
}
// we can now call Space by
Name.Space.Foo() // its static part
Name.Space sp()  // its object part
sp.Foo2()
```
for namespace unwrap we can use the using keyword similar to C# as it is short and works well in large applications
``` c++
/// in different file we can add
using Name

Space.Foo() // now we can use Space container as if we are inside Name container
Space sp()
sp.Foo2()
```
Duck supports unrestricted container extensions
``` c++
// file1.dk
Name // extending container Name with foo function
#{  
  Foo(){}
}
```
``` c++
// file2.dk
Name // extending container Name with foo2 function that calls function foo that has been declared in file 1
#{  
  	Foo2()
    {
        Foo()
    }
}
```
## Inheritance in Duck 
Duck is using the : symbol for declaring inheritance similar to C# or C++. we allow for unlimited container parents separated by , symbol
``` c++
Animal
{
  Walk(){}
}

Cat : Animal  // Cat inherits Animal's public and protected members
{ 
}

Cat cat()
cat.Walk() // this calls Walk() declaration in Animal
```
Overriding is not declared. Any protected or public function can be overridden. A virtual pointer gets generated depending if someone has overridden any function.
``` c++
Animal
{
    Walk(){}
}

Cat : Animal // Cat inherits Animal's public and protected members
{ 
    Walk()
    {
        base.Walk() // this overrides Walk() declaration in Animal
    } 
}
Cat cat()
cat.Walk() // this calls Walk() declaration in Cat
```
multiple Parents
``` c++
Animal
{
    Walk(){}
}
Consumer
{
    Eat(){}
}
Cat : Animal, Consumer
{
  HaveADinner()
  {
     Walk()
     Eat()
     Walk()
  }
}
```
Multiple inheritance gives the level of freedom that we want. Parent priority is from left to right, in case of overlap the left parent has priority. We do not recommend overlapping parents. There is still a way to access functions from inside of the container but to the outside functions with lower priority are only accessible through reflection.
``` c++
Animal
{
    Walk(){}
}
Human
{
    Walk(){}
    Eat(){}
}
Cat : Animal, Human  // in this case priority is from left to right. if a function is already defined it simply can't be re-defined in a second parent.
{
  HaveADinner()
  {
     Walk() // Walk() from Animal
     Eat()
     base.Walk() // Walk() still from Animal
     this.Human.Walk() // Walk() from Human - using this reflection container
  }
}
```
If extending a container that has inheritance it must be declared in all files to keep visible the fact that it is inheriting something. In this case it should be IDE's work to keep this fact consistent and auto fill to all files if changed.
``` c++
// file1.dk
Cat : Animal 
{
}
```
``` c++
// file2.dk
Cat   // <-- invalid, this must be "Cat : Animal"
{ 
}
```
## Interfaces in Duck 
If we want to expose a set of functions to be used by the user, we can make a function definition container.
``` c++
PredefinedFunctions
{
   Foo(){}
   Foo2(){}
}
WantToUseFunctions 
#{
   UseFunctions(PredefinedFunctions interface)
   {
        interface.Foo()
   }
}

SomethingElse {}      // to show that inheritance still works
Duck : SomethingElse, PredefinedFunctions
{
    Foo()
    {
        Print("actual implementation")
    } 

    Foo2()
    {
        Print("actual implementation")
    }
}

Duck d()
WantToUseFunctions.UseFunctions(d)
```
## Duck typing in Duck
Duck is using its reflection system to call functions in runtime, We can call any function on any container by using the .. symbol

``` c++
Example
{
}

Example e()
e..Foo() // Duck call

```
If the function is not present, the call results in a runtime error.


``` c++
Example
{
}

Example e()
if e..?Foo() // check if Foo is valid
    Print("Foo is valid") // will not print as Foo is not present in Example

?e..Foo() // if function is not present it will not execute

```

`e..?Foo()` answers whether that exact call would run, without evaluating its arguments. `e..?Foo`, with no parentheses, only asks whether any public method of that name exists.

The lookup uses the object's runtime type, so a parameter declared as `Entity& e` finds `Tick` on a `Duck`. Protected methods are not visible to duck calls, and a method with the right name but a different signature counts as missing. A skipped `?e..Foo(Bar())` does not evaluate `Bar()`, and if `Foo` returns a value the guarded call reads as that type's default.

The common use case is where we have Tick function that we want to call on anything in the world that has it implemented, to keep the Entity as light as possible we might not want to add the Tick function there.
``` c++
Entity
{}

World
{
    Entity [] entities

    Tick(float deltaTime)
    {
        for en : entities
        {
            ?en..Tick(deltaTime)
        }
    }
}

Duck : Entity
{
    Tick(float deltaTime)
    {
        Print("Duck tick")
    }
}

Human : Entity
{
    // no tick
}

World w()
Duck d()
Human h()
w.entities += d
w.entities += h
w.Tick(0.1) // Prints Duck tick

```

## Enums in Duck

Duck uses an inheritance-style declaration for enums. In `Day : byte`, `byte` is the underlying type. The `#` marks the block as static, and `@` marks it as an enumeration.

Enums are an exception to Duck's container chaining rule. Declare every member in one block. A second block for the same enum is a compile error, even across files. In practice, merging blocks hid accidental name collisions and could change the values assigned to members.

``` c++
Day : byte 
#@{
    Sunday, Monday, Tuesday, Wednesday, Thursday, Friday, Saturday
}
```
The same declaration can spell out the values explicitly.

``` c++
Day : byte 
#@{
    Sunday: 0, Monday: 1, Tuesday: 2, Wednesday: 3, Thursday: 4, Friday: 5, Saturday: 6
}

Print(Day.Sunday) // prints Sunday
Print(Day.Sunday == 0) // prints true  <-- direct cast

```
in Duck you can also make enums out of non-traditional values such as floats and strings
``` c++
IrrationalNumbers : float
#@{
    Pi: 3.141592653589,
    SqRoot2: 1.4142135623, 
}

Increments : float 
#@{
    I1,I2,I3,I4 // the default values for float starts at 0 and increment by 0.5
} // I1 = 0, I2 = 0.5, I3 = 1.0, I4 = 1.5
```
String enum also retains the Enum name when prints
``` c++
CommonNames : string 
#@{
  Bob: "Bob",
  Tom: "Thomas",
  Dan: "Daniel"
}

Print(CommonNames.Tom) // prints Tom
Print(CommonNames.Bob + " and " + CommonNames.Tom) // prints Bob and Thomas <-- direct cast  
```
A value can be a constant expression that uses other members
``` c++
Access : int
#@{
    None, Read: 1 << 0, Write: 1 << 1, All: Read | Write
}
```
Converting back to an enum is explicit. `(Day)n` gives the member that holds `n` and stops the program if there is none, so `IsValid((Day)n)` can ask first. Every enum value also knows its member's name and value
``` c++
Day d = (Day)2
Print(d.Name)            // prints Tuesday
Print(d.Value)           // prints 2
Print(IsValid((Day)42))  // prints false
```
A for loop visits every member of an enum, in the order they were declared
``` c++
for day : Day
{
    Print(day)           // Sunday, Monday, Tuesday, ...
}

Print(Day.Count)         // prints 7, the number of members
Print(Day.Min)           // prints Sunday, the member with the smallest value
Print(Day.Max)           // prints Saturday, the member with the largest value
```
Enums can also live in a static block, where they are named by the container
``` c++
Game
#{
    Mode : byte
    #@{
        Menu, Play, Paused
    }
}

Game.Mode mode = Game.Mode.Play
```

`Print(null)` flushes buffered output immediately and writes no characters or
newline.

## TCP networking

On Windows, Duck provides `Net`, `TcpListener`, and `TcpConnection`. The compiler
links the networking runtime automatically. Keep `duck_net.lib` beside `duckc.exe`
when moving the toolchain.

```c++
listener : Net.ListenTcp("127.0.0.1", 0)
if !listener { return }
client : Net.ConnectTcp("localhost", listener.Port())
if !client { return }
```

- `ListenTcp(address, port)` binds a local address. Port zero selects an available
  port. `Port()` returns the actual port. Listener setup resolves its address synchronously.
- `ConnectTcp(host, port)` starts hostname lookup and connection establishment
  asynchronously. Poll `client.Status()` for 0 (pending), 1 (connected), or
  -1 (failed or locally closed). Either factory can return null on setup failure.
- `listener.Accept()` returns an owning `TcpConnection*` when a client is ready.
  A null result with `listener.AcceptStatus == 0` means try again later.
  A status of -1 means the accept failed.
- `connection.Read(buffer)` and `connection.Write(buffer)` take a nonempty
  dynamic `byte []`. A positive result counts bytes transferred, -2 means try
  again later, and -1 indicates an error. A zero-byte read means the peer ended
  its send stream. Read into a buffer sized before the call. TCP is a stream,
  so accumulate partial reads and writes. `ReadAt(buffer, offset)` and
  `WriteAt(buffer, offset)` resume at a byte offset. Each call transfers up to 4096 bytes.
- Both handle types close on destruction. Call `Close()` to release one early.
  An accepted connection remains usable after its listener closes.


## UDP networking

On Windows, `Net.BindUdp(address, port)` returns an owning `UdpSocket*`.
Bind to a numeric IPv4 or IPv6 address. Port zero assigns a free local port,
available through `Port()`. A failed bind returns null.

```c++
socket : Net.BindUdp("127.0.0.1", 0)
if !socket { return }
UdpEndpoint peer = Net.UdpAddress("127.0.0.1", 9000)
byte [] message = {1, 2, 3}
int sent = socket.SendTo(message, peer)
```

Each `SendTo(data, destination)` sends one datagram. Each
`ReceiveFrom(buffer, sender)` reads one datagram into a presized dynamic
`byte []`. On success it also fills the `UdpEndpoint` with the sender's address
and port. Pass that endpoint directly to `SendTo` to reply. Both operations
are nonblocking.

| Result | Meaning |
|---|---|
| 0 or greater | Datagram size in bytes. Zero is a valid empty datagram. |
| -2 | Would block. Try again later. |
| -3 | Datagram too large. On receive, the socket discards that packet and leaves the buffer and sender unchanged. |
| -1 | Closed socket, invalid destination, or another socket error. |

`Net.UdpAddress(address, port)` accepts numeric addresses, including IPv6
scope IDs such as `fe80::1%13`. A valid endpoint has `Family` 4 or 6,
`Address` bytes in network order (4 or 16 bytes), `Port` in host order, and
an IPv6 `ScopeId`. Family zero indicates a parse failure. Use a destination
of the same family as the bound socket. Call `Close()` to release the socket
early, or let its owner go out of scope.

## Networking callbacks

Register a handler on a socket to start its standard-library receive worker.
The first registration creates one background thread for that socket. Handlers
on the same socket run in sequence. Handlers on different sockets can overlap
each other and the game thread.

```c++
settings : Net.ReceiveSettings()
settings.SleepMs = 2
settings.TcpChunkBytes = 8192
Net.SetReceiveSettings(settings)

socket : Net.BindUdp("127.0.0.1", 9000)
if !socket { return }
socket.OnReceive(@ (NetReceiveEvent& packet) {
    Print(packet.Status)
})
// Keep the socket owner alive while the application wants callbacks.
```

| Registration | Handler argument | Delivery |
|---|---|---|
| `connection.OnConnect(handler)` | `int status` | Once, with 1 on success or -1 on failure. Works when registered after the connection has completed. |
| `listener.OnConnect(handler)` | `NetIncoming& incoming` | Once per accepted connection. Use `client :: incoming.Connection` to transfer ownership if the connection should outlive the handler. |
| `connection.OnReceive(handler)` | `NetReceiveEvent& packet` | Each available TCP chunk. A final status of 0 means peer shutdown and -1 means failure. |
| `socket.OnReceive(handler)` on UDP | `NetReceiveEvent& packet` | One datagram, including empty datagrams. `packet.Sender` holds its endpoint. A negative status ends delivery. |

`packet.Data` is a dynamic byte array and `packet.Status` gives the number of
bytes on a successful receive. Callback arguments are valid for the duration of
the call. Registration returns a boolean. A closed socket, null handler,
duplicate registration, or failure to start the worker returns false.
A registered receive or accept handler owns that operation until close.
Writes and UDP sends can still be made from another thread.

`Net.ReceiveSettings()` returns a snapshot of the process-wide settings.
`Net.SetReceiveSettings(settings)` applies both fields together and returns
false for an invalid value. `SleepMs` controls the idle retry interval
(default 5 ms, range 1 to 60000). `TcpChunkBytes` controls the largest TCP callback
payload (default 4096, range 1 to 65536). Workers read the settings on their
next iteration and drain available data before sleeping.

The library synchronizes socket operations and keeps the registered Duck
closure alive. Use Duck's `${ ... }` blocks or another synchronization method
for application data shared with a callback. `Close()` wakes the worker and,
when called outside a networking callback, waits for it to finish. Release
locks needed by the callback before calling `Close()` or dropping the last
socket owner. Inside a networking callback, closing a socket requests shutdown
and allows an in-flight handler to finish.

The [ping server](Examples/Networking/PingServer.dk) and [ping client](Examples/Networking/PingClient.dk)
are separate programs. Build each with [Ping.dk](Examples/Networking/Ping.dk),
start the server, and pass its printed port to the client. Both use two-byte
TCP receive chunks to assemble the four-byte messages across callbacks.

## Browser WebTransport

On `wasm64`, `Net.ConnectWebTransport(string url)` opens a session to an HTTPS
WebTransport server that supports unreliable datagrams. `session.OpenStream()`
opens a reliable bidirectional stream. Both return owning handles immediately,
or null on setup failure. A stream can be requested while its session connects.
Keep the owners alive across browser event-loop turns. The
[echo client](Examples/Projects/WebTransport/Client.dk) uses a named scope to
keep its session and stream alive after `Main` returns.

| Operation | Behavior |
|---|---|
| `session.Status()`, `stream.Status()` | 0 pending, 1 ready, -1 closed or failed. |
| `session.OnConnect(handler)`, `stream.OnConnect(handler)` | Calls a `func (int)` once with the opening result (1 or -1). Late registration receives the result on a later turn. |
| `stream.OnReceive(handler)` | Calls a `func (NetReceiveEvent&)` for each chunk, in order. Positive `Status` counts bytes in `Data`. A final 0 means the peer finished writing, and -1 means receive failure. A chunk can contain part of a message or several messages. |
| `session.OnDatagram(handler)` | The same handler signature delivers one whole datagram. Zero means an empty datagram. A final -1 ends delivery. |
| `stream.Read(buffer)`, `stream.Write(buffer)` | Transfer up to 4096 bytes per call. `ReadAt(buffer, offset)` and `WriteAt(buffer, offset)` resume at a byte offset. Return the count, -2 to retry on a later turn, or -1 on error. A zero-byte read means end of stream. |
| `stream.FinishWrite()` | Queue FIN after accepted writes. Returns true once. The peer can still send a reply. |
| `session.SendDatagram(data)`, `session.ReceiveDatagram(buffer)` | Return the whole datagram size, -2 to retry, -1 on error, or -3 for an oversized datagram. An undersized receive buffer discards that packet and leaves the buffer unchanged. `MaxDatagramSize()` gives the send limit when ready. |

Handlers run on the browser event loop. Registration returns false for a null
or duplicate handler or a locally closed handle. An installed receive handler
owns reads on its stream, or datagram reads on its session. Polling that same
operation then returns -1. Each handle allows one outstanding write and copies
the accepted bytes before returning. A successful write reports local
acceptance. Datagram delivery can be lost or reordered.

`Close()` and owner destruction cancel subsequent callbacks. The active handler
may close or release its own handle. Closing a session also closes its streams.
Callback data is valid during the call.

## Files in Duck
`file` is a built-in type for reading and writing files on disk. A file starts closed, cannot be copied, and closes itself when it goes away, so `Close()` is only needed to finish early.
``` c++
file f
if f.Open("level.txt")              // false if it cannot be opened
{
    string text = f.ReadText()      // the rest of the file as text
}

uint8 [4096] chunk
long n = f.Read(chunk)              // raw bytes; 0 at the end

file log
log.Create("log.txt")               // Append(path) adds to the end instead
log.Write("started\n")
```

## Properties in Duck
the @ symbol is used as representation to similar to Get, with inclusion of any non-static variable in @ block, by default anything after : is returned
``` c++
Person 
{
  -int Age = 20 // protected variable age
}
@{ // Get export for age // non-static block
  Age: Age              
}

Person p()
Print(p.Age)      // prints 20 
Print(Person.Age) // prints Person has no property Age
// static block for age has not been defined
```
this also allows expressions
``` c++
Population 
{
      -Ages : {20,50,12,23,40}  // protected array ages
}
@{
  MaxAge: Ages.Max, // returns 50
  FirstAge: ?Ages[0] 	// checks if index 0 is valid and returns that value now it is 20, if index 0 is not valid it returns the default value of int, 0
}
```
Setter are similarly set inside the ={} block by
``` c++
Person 
{
  -int Age = 20 // protected variable age
}
={
  Age: Age = value // Set age variable with value on the right hand side of =
}

Person p()
p.Age = 25
Print(p.Age)     // prints 25 
``` 

## Bitwise and shift operators in Duck
Similar to c++ or C#, Duck is using standard ~, <<, >>, &, |, and ^ symbols for work with bits.
The bootstrap implements these operators for integers, together with `&=`,
`|=`, `^=`, `<<=`, and `>>=`. Shifts preserve the left operand's width;
negative counts or counts at least that width trap. Signed right shifts
extend the sign bit; unsigned right shifts insert zeros. Bitwise operators
use C precedence. In a conditional true arm, parenthesize OR to distinguish
it from the separator: `ready ? (a | b) | c`.
``` c++
uint x = 0x05		// 00000101
Print(x & 0x03) 	// 00000001  <-- Bitwise AND   
Print(x | 0x03)		// 00000111  <-- Bitwise OR
Print(x ^ 0x03) 	// 00000110  <-- Bitwise XOR
Print(~x)		    // 4294967290 (uint32) <-- Bitwise NOT
Print(x << 1)		// 00001010  <-- Left shift 
Print(x >> 1)		// 00000010  <-- Right shift 
``` 
you can also use them in assignment
``` c++
uint x = 0x05
x &= 0x03
Print(x)  	// 00000001 
``` 
## Math in Duck
`Math` is a namespace built into the compiler, so it needs no import and no
native library. Programs cannot declare their own container called `Math`.

``` c++
float root = Math.Sqrt(16.0)
int biggest = Math.Max(3, 7)
float limited = Math.Clamp(value, 0.0, 1.0)
```

| Takes | Functions |
| --- | --- |
| float or double | `Sqrt` `Cbrt` `Exp` `Exp2` `Log` `Log2` `Log10` `Sin` `Cos` `Tan` `Asin` `Acos` `Atan` `Sinh` `Cosh` `Tanh` `Floor` `Ceil` `Round` `Trunc` `Pow(a, b)` `Atan2(y, x)` `Hypot(a, b)` `Mod(a, b)` `Lerp(a, b, t)` |
| any fixed-width number | `Abs` `Sign` `Min(a, b)` `Max(a, b)` `Clamp(value, low, high)` |
| vector containers | `Dot(a, b)` `Cross(a, b)` `Length(v)` `Distance(a, b)` `Normalize(v)` |

All operands take one common type and the result has that type, so
`Math.Sqrt(f)` on a float is a float and on a double is a double. The functions
that need a fraction turn integer operands into double, or into float when the
result is assigned to a float. `Round` rounds halves away from zero, and integer
`Abs` wraps on the most negative value like other integer arithmetic.

A vector is any container whose fields are exactly
`X`, `Y` and optionally `Z` and `W`, all float or all double. `Cross` needs all
three of `X`, `Y`, `Z`. `Cross` and `Normalize` return the same container, and
`Normalize` leaves a zero vector at zero.

``` c++
Vec3
{
    float X
    float Y
    float Z
}

Vec3 up = Math.Cross(right, forward)
float facing = Math.Dot(up, normal)
```

Number functions also work in compile-time expressions: `#Math.Pow(2.0, 8.0)`.

## Lambda functions in Duck
Lambda functions are initialized by @ symbol before, indicating the start of the Lambda function. We can use func type to store function reference.
``` c++
Lambda
{
   // standard declaration
   int Foo()
   {
        return 5
   }

   // Lambda function with return
   func F =  @ int(){
        return 5
   }
   
   // Lambda function takes in references to resources from outside if they are needed.
   func F2 = @ (){
        Print(F()) 
   }
}

Lambda l()
l.F2()  // prints 5
``` 
A `func` has one signature for its whole life, taken from its initializer or written out, and every call through it is checked against that signature at compile time. Parameters, results, and fields without a lambda default must write it out.

``` c++
int Add(int a, int b)
{
    return a + b
}

int Apply(func int(int, int) operation, int a, int b) // result first, then parameters
{
    return operation(a, b)
}

func int(int, int) add = Add      // a global or static function is a value too
Print(Apply(add, 2, 3))           // 5
Print(Apply(@ int(int a, int b) { return a * b }, 2, 3)) // 6

func int() nothing = null
if nothing == null
    Print("calling a null func is a runtime error")
```

Arrays can hold `func` values. Every element has the array's signature, and an element is called like any other func. A fixed array starts with null elements.

``` c++
func int(int) [] steps = {Double, @ int(int x) { return x + 1 }}
steps += @ int(int x) { return x * x }

int value = 3
for step : steps
{
    value = step(value)
}
Print(steps[0](10)) // calls the first element
```

Lambdas capture locals by reference and keep them alive, so a lambda can safely outlive the scope that created it.

``` c++
func int() MakeCounter()
{
    int count = 0
    return @ int()
    {
        count += 1
        return count
    }
}

counter : MakeCounter()
counter() // 1
counter() // 2
```

A lambda never owns a tracked pointer. Capturing an owner or an alias captures an alias, which expires when the owner lets the resource go, and which the lambda cannot reassign.

A lambda that uses `this`, a field, or a reference parameter depends on the current call's storage, so it must be stored directly in a local and can only be called there. A lambda written as a field's default is the exception: it runs against whichever object it is called on, so it is always called through its object (`l.F2()`) and cannot be read as a value.

## Threading control in Duck
Duck synchronizes resource use with preceding work affecting that resource. For example, reading `c.Value` below waits for both earlier increments to finish, so the printed value is guaranteed to be 2.

``` c++
Counter
{
    int Value = 0

    Increment()
    {
        Value++
    }
}

Counter c()
c.Increment()
c.Increment()
Print(c.Value) // always prints 2; resource use is a synchronization point
```

This does not make every sequence of accesses atomic. Without an explicit guarantee, two withdrawals could both pass a balance check before either subtracts money. The compiler may execute small calculations sequentially, but programs must not rely on that optimization for correctness.

A scope prefixed with `$` guarantees ordered execution and locks the resources used by the block, including resources accessed through called functions. Earlier work touching those resources must finish before the block starts. Other threads cannot modify those resources while the block runs. Each statement's work on those resources finishes before the next statement executes, and the locks are released when the block finishes.

``` c++
${
    c.Increment()
    Print(c.Value) // no other thread can modify c.Value between these statements
}
```

Use `$` around a check and its dependent modification to guarantee that they execute together without interference. The bank example also locks both accounts for the ordered transfers.
``` c++
Bank
{
    int Cash = 5
   
    AddCash(int amount)
    {
      Cash += amount
    }
    
    int Withdraw (int amount)
    {
      ${ // protect the balance check and subtraction together
       if Cash - amount >= 0
       {
           Cash -= amount
           return amount
       }
       else
       {
          int amountLeft = Cash
          Cash = 0
          return amountLeft
       }    
      }
    }
    
    TransferMoneyToBank(int amount, Bank bank )
    {
       gotAmount : Withdraw(amount)
       bank.AddCash(gotAmount)
    }
    
    Foo()
    {
       Bank a
       Bank b
       
       ${ // ordered execution with locks for resources accessed in both accounts
          a.TransferMoneyToBank(5,b) 
          b.TransferMoneyToBank(10,a)  // guaranteed final balances: a.Cash = 10, b.Cash = 0
       }
       
    }
}
``` 
### Explicit parallel branches

`P` groups two or more branches into one statement. Each pair of braces encloses one branch, which can contain several statements. Each pair of parentheses takes a zero-argument callable, which the group invokes once. The forms can be mixed in any order.

```c++
// Four ways to run the same two branches
P{ X() }{ Y() }
P(X)(Y)
P{ X() }(Y)
P(X){ Y() }
```

A branch can also be a lambda. The next statement runs after every branch has finished.

```c++
P{
    UpdatePhysics(dt)
    ResolveContacts()
}(MixAudio)(@ () { Animate(dt) })
SubmitFrame()
```

Here the physics branch resolves contacts after updating physics. Audio and animation can overlap that work. The explicit group overrides the compiler's cost estimate. The compiler must still establish that the branches can safely overlap, including their use of captured variables and resources accessed by their callees. If it cannot, it reports a compile-time error. The runtime worker limit still applies.

See [the runnable parallel-branch example](Examples/Concurrency/11_ParallelBranches.dk) and [the bootstrap compiler’s scheduling rules](Compiler/README.md#explicit-parallel-groups).

### Inspecting scheduler threads

`P.ThreadCount` reports live Duck scheduler threads, including threads waiting on a join or an ordered block. `P.RunningThreads` counts those outside either wait. Slot zero belongs to the main thread. Use `P.ThreadSlots` to enumerate slots, since a finished worker leaves an idle slot that a later worker can reuse.

```c++
int slots = P.ThreadSlots
for int i = 0; i < slots; i++ {
    int status = P.Thread[i].Status
    if status == P.WaitingForJoin { Print(i) }
}
```

The read-only integer states are `P.Idle`, `P.Starting`, `P.Running`, `P.WaitingForJoin`, and `P.WaitingForLock`. A wait observed across several samples can help locate a stall. Each read samples current state. By the time execution continues past a `P` group, all its branches have finished. [Runtime details](Compiler/README.md#thread-inspection)

## Reflection system in Duck
The main reason for reflection system in Duck is to allow for the creation of Game Engine visualizations based on the code. the reflection system can be accessed using this keyword and the name of the container.

``` c++
Example
{
    Foo()
    {
        Print(this.Example.GetName()+ "->" + this.Example.Foo.GetName()+"()")
    }
}

Example e()
e.Foo() // Prints Example->Foo()

``` 
We can add custom data into the reflection system with [], similar to C#

``` c++

[args]   // first we mark the enum that we want to be used as part of the custom data
Visibility : byte 
#@{
    Hidden, VisibleNonEditable, VisibleEditable
}


Example
{
    [VisibleNonEditable]
    int Money = 0

    [Hidden]
    AddCash(int money)
    {
        Money += money
    }

    CanWeSeeMoney()
    {
        byte [] tags = this.Example.Money.GetTags()

        if tags.Len > 0 && tags[0] == Visibility.VisibleNonEditable
        {
            Print("We can see the money")
        }
    }
}

``` 

Only enums marked `[args]` can be used as attributes. A member can carry several, as in `[Hidden, Slow]`, and `[Visibility.Hidden]` qualifies a name two enums share. Attributes apply to instance fields and methods.

`GetTags()` returns a dynamic array. The declared element type selects the attributes: `byte []` keeps those whose enum is stored as `byte`, and `Visibility []` keeps that enum's members as enum values. Without a declared type, all of the member's attributes must share one underlying type. An override reports its own attributes.

`GetName()` gives a `string`, or a `name` when one is expected, and containers report their qualified name. `this.Player.Reset.GetDeclaringName()` gives the container that declared a member, which for an inherited member is its parent. A parent's path works too, as in `this.Entity.GetName()`, and a method the parent declares with one of these names takes priority.

## Unit tests in Duck
Unit tests are part of the core code as Duck will use them to gauge correctness and speed while optimizing the compiled code for multithreading, this is sort of to force users of Duck to make tests as part of their workflow,
Test block starts with the T Block, then testing functions are accessed using the T Container

``` c++
Example
{
    int Foo()
    {
        return 3
    }
}
#{
    int Foo2()
    {
        return 5
    }
}
T{ // <- Testing block
    Test1() // 
    {
        T.AreEqual(Example.Foo2(), 5)
    }

    Test2()
    {
        Example e()
        T.IsTrue(e.Foo() == 3)
    }
}


``` 

A `T{` block, written without a space, follows one of a container's bodies with no blank line between them. Each member is a test: a name, an empty parameter list, and a body. Tests can use anything the container's own members can, including protected members. A test's stable name is its container's qualified name and its own, such as `Example.Test1` or `Outer.Inner.Test1`. Two tests with one name in a test build are an error, and the name is what `--list` prints and filters match. `T {` with a space is still an ordinary container named `T`.

Tests run apart from the program. An ordinary build leaves `T{` blocks out after parsing them, so its output is byte for byte the same with or without tests, and test bodies are checked only in a test build. A test build replaces the entry point with a generated runner, and `Main` is compiled but never called.

``` powershell
# One file or a set of files
duckc --test Example.dk -o Example.test.ll
clang Example.test.ll -o Example.test.exe
.\Example.test.exe                 # run every test
.\Example.test.exe --list          # print test names
.\Example.test.exe Example.Test1   # run tests whose name contains this text; repeat for more

# A project: one test executable per module, under <build>/<profile>/tests
duck test Game.dkp
duck test Game.dkp --module core --filter Example.Test1
```

`--timing` adds each test's elapsed nanoseconds, and `duck test Game.dkp --compare-schedules` uses that to compare the sequential schedule with the parallel ones the compiler's dependency analysis accepted. A schedule that fails a test is discarded. Tests can only reject or prefer such a schedule: passing them cannot show that it is free of races.

The runner exits with `0` when every selected test passed, `1` when any failed, and `2` for bad usage or filters that match nothing.

| Assertion | Fails when |
| --- | --- |
| `T.AreEqual(actual, expected)` | `actual == expected` is false |
| `T.AreNotEqual(a, b)` | `a == b` is true |
| `T.IsTrue(condition)` | the condition is false |
| `T.IsFalse(condition)` | the condition is true |
| `T.Fail()`, `T.Fail("reason")` | always |

Assertions are statements, available directly in a test body, including inside its loops, branches, and macros, but not inside a lambda. Each operand runs once, and the comparison accepts exactly what `==` accepts. The first failed assertion reports and ends that test through its normal cleanup, and the run moves on to the next test:

``` text
RUN  Example.Test1
C:\src\Example.dk:17:9: test Example.Test1: T.AreEqual failed
  actual:   5
  expected: 6
FAIL Example.Test1
0 passed, 1 failed, 1 total
```

Values `Print` cannot show, such as pointers, are compared but left out of the report. Tests share one process and its static fields, in name order, and a test that traps stops the run; its `RUN` line is the last one printed.

## Markup in Duck
Markup is based on XML style markup where we have single root node and elements of that node. Duck allows to use normal code along with markup to achieve similar capabilities to JSX in React or WPF, inside object block in container we define block using this keyword. root node can be set or omitted and is always the name of the container

``` c++

Example2
{
    string Name
    int Age
}


Example
{
    this{
    // <Example>
        <Example2 Name="example" Age=5/>
    // </Example>
    }

    Foo()
    {
        Print(this.Example2.Name) // we get access to the component using this and it has the same name as the container
        Print(this.Example2.Age)
    }
}


``` 

If we have multiple child elements or an unknown number of them we can check them using the GetChildren() function
``` c++

Guy
{
    string Name
    int Age
}

Example
{
    this{
    // <Example>
        <Guy Name="bob" Age=18/>
        <Guy Name="rob" Age=21/>
    // </Example>
    }

    Foo()
    {
         Print(this.GetChildren().Len) // Prints 2
         Print(this.Guy.Name) // <-- Error Component has duplicates
         Print(this.Guy[0].Name) // Prints bob
    }
}
``` 

We might want to also generate the Components using functions
``` c++
Guy
{
    string Name
    int Age
}
#
{
    markup GetGuyComponent(string who, int age)
    {
        return {
            <Guy Name=who Age=age>
            "name age" // we do things differently here from XML values have to be declared as literals or any supported type like string, int and so on, the resulting value variable will have that type. escaping is done using the \ character
            </Guy>
        }
    }
}


Example
{
    this{
    // <Example>
        Guy.GetGuyComponent("bob",18)
        Guy.GetGuyComponent("rob",21)
    // </Example>
    }

    Foo()
    {
         Print(this.Guy[0].value) // Prints name age
    }

}

``` 
Tags name container types, and each attribute is an ordinary assignment to a field of that container, so its value is checked against the field. Attribute values stop at any operator, which keeps `/>` from being read as a division: write `Height=(Base + 1)` for an expression. Elements nest, close with `</Tag>` or `/>`, and are built when their container is constructed. A `this{ }` block may read the container's own fields.

- **Finding children.** `x.Tag` is the only child of that type. Repeated children need an index, `x.Tag[0]`. Where the compiler can see every child, a repeated or missing `x.Tag` is a compile error; once a function adds children it traps at run time instead. A field of the same name takes priority. `?x.Tag[5].Foo()` skips a missing child.
- **Listing children.** `x.GetChildren()` is a view in declaration order. It has `Len`, can be indexed, and can be looped over. Each element has `GetName()` and `GetChildren()`, and `..` calls its container's methods.
- **Values.** A literal inside an element gives it a `value` field of the literal's type. To store anything else, declare `value` and write the expression, as in `<Guy>text</Guy>`.
- **Markup values.** `markup` holds elements with no parent yet. A `{ ... }` is markup where a markup type is expected or where its first content is a tag. Like a collection, it moves rather than copies. Writing one inside other markup moves its elements to the end of that spot, in order, and leaves the original empty.
- **Lifetime.** A container owns its children and releases them when it is destroyed, which makes it move-only. A container defined in another module can hold children or a literal value only if its own module gave it the storage.

## Compile time execution in Duck
Prefix a direct call with `#` to evaluate it during compilation (also called
baking). The bootstrap supports bounded evaluation of scalar Duck functions
with constant arguments and local effects. See the [supported effects and
limits](Compiler/README.md#explicit-compile-time-calls).
``` c++
Example #{
    int Foo(int i) 
    {
        return i > 0 ? 16 * i + Foo(i - 1) | 0
    }
    int Foo2()
    {
       int fooResult = #Foo(5) // <-- this gets replaced by whatever Result of this function is at compile time.
       fooResult++
       return fooResult
    }
}
```

## Macros in Duck
macros in C++ are extremely powerful tool but also completely impossible to debug and read, Duck solves this by functions that get unwrapped at compile time, functions have to return macro type. We force at least some sort of structure by having arguments of the function statically typed.

``` c++
Example 
#{

    macro Foo() // anything is allowed. syntax correction gets checked only when used
    {
      case 0:  
        Print("Hello world")
        break
    }

    Foo2()
    {
        int index = 0

        Switch (index)
        {
            Foo()
        }
    }

}
```

A macro can be declared at file scope or in a container body, but not inside a function. Macros are collected before parsing, so calls may come before the definition or from another file of the module. A macro is checked only where it is expanded, and a failure inside macro output names the macro body, the definition, and every call that produced it.

- **Arguments are typed and bound once.** Each parameter becomes a read-only local, so an argument is converted to the declared type and evaluated exactly once, in order, even if the body never names it. Parameters cannot be references; a body can name the caller's variable directly.
- **Hygiene.** Names a macro declares (locals, loop variables, lambda parameters, and its parameters) are private to that expansion: they cannot capture the caller's names, and the caller cannot see them. Every other name is looked up where the macro is expanded, which is what lets `case`, `break`, and the caller's own variables work. A macro called from a macro is looked up from the macro's own container first.
- **Limits.** Macros cannot be recursive. Expansion stops at 32 nested levels and 200000 tokens per file.

The position of the call decides what the macro must produce:

| Position | Produces | Argument binding |
| --- | --- | --- |
| A statement | A block. `break`, `continue`, and `return` still act on the caller. | Locals at the start of the block |
| Directly in a `Switch` body | `case` sections | Locals on the `Switch`, after the selector is evaluated |
| Inside an expression | One expression | Locals scoped to that expression, evaluated only when it is reached |

A parameter can be used any number of times in every position, and its argument still runs once. Macros cannot produce declarations.

## Switch in Duck
`Switch` evaluates its selector once, then compares it with each label in source order. `default` runs only when every label misses, wherever it is written. Without a match or a `default`, nothing runs.

``` c++
Switch (day)
{
    case Day.Saturday:      // Stacked labels share one body.
    case Day.Sunday:
        Print("weekend")
        break
    case Day.Monday, Day.Friday:
        Print("almost")
        break
    default:
        Print("weekday")
        break
}
```

There is no implicit fallthrough: every section must end in `break`, `continue`, or `return`, or the program does not compile. `break` leaves the innermost `Switch` or loop, and `continue` goes to the enclosing loop.

The selector is an integer, `bool`, `char`, enum, `string`, or `name`. Labels convert to the selector's type and must be literals, enum members, type limits such as `int.Max`, `#Call(...)`, or `#` constants. Two labels with the same known value are rejected.

## Definitions in Duck
defined constants can be used only by the #if, this is to prevent C style defines without type

A `#define` holds a `bool`, integer, or string, and can be built from earlier defines with `!`, `-`, `&&`, `||`, parentheses, and comparisons. A define is visible from its line to the end of its file. Other files never see it, redefining it is an error, and a name that is not defined yet is an error rather than false. A `#if` condition must be a `bool`.

`#if` and `#else` run before any declarations are collected, so an inactive branch publishes no names and only has to balance its braces. The active branch's braces are removed, so its declarations land in the surrounding scope. `#if` also works inside container and function bodies.

``` c++

#define WIN_64 true

#if WIN_64
{
    Example 
    #{
        Foo()
        {
            Print("x64")
        }
    }
}
#else
{
    Example 
    #{
        Foo()
        {
            Print("x32")
        }
    }
}


```

## Life cycle in Duck
Common program structure in C++ is to have some kind of Start function that allocates stuff on the heap, Like loads world that run for the duration of the program's life, or if switched destroyed and re-initialized with new world. The issue is that most objects in the world need to get initialized under the world, this may be true for some that want a reference back to the world but not for all. Or if we have start and stop functionality where we add new objects to the world and want to remove them at the end of play.

Duck solves this with custom scopes where we create large scope and we allocate marked variables on arena allocator. then we can start and end the scope at specific points of the program.

``` c++
PlayManager
{
    [@Play]
    float PlayTime

    Tick(float deltaTime) // called from main loop
    {
        //playTime += 0.01 // <-- runtime error if not allocated
        ?PlayTime += deltaTime  // will not execute if not allocated
    }

    StartPlay() // used by Button on UI Or Command
    {
        @start Play
    }

    StopPlay() // used by Button on UI Or Command
    {
        Print(PlayTime)
        @end Play
    }
}
```
if end will never get called, it will be called at the end of the program.

We can also use this in the static block making the variables not get allocated at the start of the program but at the point of custom scope
``` c++
PlayerManager
#{
    [@Play]
    Player [] Players
}

Player
{
    Foo()
    {
        Print(PlayerManager.Players.Len)
    }
}

World
{
    AddPlayer(Player player)
    {
        PlayerManager.Players += player
    }
}


```
if we use dynamic collections it will get allocated using the arena allocator

After Start function we usually have some kind of loop that runs forever. Each cycle of the loop represents a game frame, we might again allocate stuff at the start of the frame and dump them at the end of the frame. if we mark loop using custom scope it will automatically call the @start and the @end even if we use for example break to escape it, if we call the @end anywhere in the loop it will also escape the loop

``` c++
Example
#{
    Foo()
    {
        @end Tick
        Print("Hello") // <-- this will not run
    }
}

Main() 
{
    [@Tick]
    while true
    {
        Example.Foo()
        Sleep(0.01)
    }
}
```

you can check if the custom scope is active or not with if statement where you use the @ followed by the name of the scope
``` c++
Example
#{
    Foo()
    {
        if @Tick
            Print("Tick active")
        else
            Print("Tick inactive")
    }
}

Main() 
{
    [@Tick]
    while true
    {
        Example.Foo() // Tick active
        Sleep(0.01)
    }

    Example.Foo() // Tick inactive
}
```

if we call @start again after @start has already been called for custom scope it will re-initialize the scoped variables, if we call @end again after it has been called it will be runtime error.

Scope names are global to the program and need no declaration. A marked variable starts at its type's default the first time it is used in each run of its scope, so instances created after `@start` work the same as earlier ones, and a marked field cannot have its own initializer. Using one outside its scope is a runtime error, while `?PlayTime += d` and `?PlayTime` skip the write or read the default. Copying a container shares its scoped values.

`@end` runs the cleanup of that scope's values, newest first, while they can still be read, then frees its arena. Collections keep their elements on the general heap and release them at `@end`.

A marked loop starts its scope at the top of each iteration and ends it on every way out: reaching the end of the body, `continue`, `break`, `return`, or `@end`. The loop's condition and step run outside the scope. `@end` anywhere under a marked loop of that name, including in a function it calls, stops the code after it, cleans up each function's locals on the way out, and continues after that loop. Called functions return their type's default on the way, which nobody sees. `@end` in a lambda body is not allowed. Ending an outer scope also closes every marked loop inside it. With no marked loop running, `@end` just ends the scope and the code after it continues.

Scopes are shared by the whole process, and scheduled tasks never touch them. Only the thread running a marked loop may `@end` its scope, and other native threads must coordinate their own uses with any `@end`.
