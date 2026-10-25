# Duck
Duck is a programming language, focusing on speed and abstraction, designed mainly for game development. Duck is statically typed and compiled language. Main feature of using Duck is async and sync design is resolved by the compiler with full thread safety.
# Philosophy
There are many languages, each great in their own way, the only reason nowadays for making a new language is to do something revolutionary. As CPUs have more and more cores over time there are not many reasons to make single threaded applications, async programming can be ok but from time to time you run into race conditions, concurrency problems or just additional code to stop your threads from fighting over resources. Duck is here to save the day. As many things are inherently async: some I/O operations, code that takes too long to execute, or any network operations. Duck solves for all of this in compile time.

No garbage collector, there should not be generated any garbage in the first place. Duck has a single parent policy. At which the owning parent destroys its children or the parent gets destroyed, the resource gets removed. But until then others can reference this resource allowing for having the same resource with multiple references.

Support for reflection that can be then used while building game engine

Build in markup tags for building UI/UX

Testing included as core part of the language

Basic data collection types like strings/arrays/maps and so on, are core part of the language. This is firstly that we don't have to create our own or use something like std:: in C++. Secondly this helps duck create a multithreading specific for collections.

Duck as the name suggest supports duck typing. Why have duck typing in statically typed language? To offer alternative to polymorphism where sometimes we have function that applies to large number of classes/structs but they have nothing else in common, we would be forced to use interfaces or events to append that functionality into our inheratence structure.

Duck forces type safety for functions as they are the worse offenders in automatic return type or automatic arguments. I'm looking at you JavaScript.

# Why this syntax?
Duck syntax is based on my personal preferences and my imagination of the perfect syntax. I like type safety as in my opinion it makes code simpler to read especially as you get return from functions/method and you have to mouse over to see what it is actually returning, with type safety you see it right there. But on the other hand i'm not fan of verbose syntax, as it makes the whole code unnecessarily too large.

# Project status
Current status: **early C++ bootstrap compiler**. The language design below is
broader than the implemented subset. See [Compiler/README.md](Compiler/README.md)
for build instructions, executable examples, and current limitations.
The separate [build tool](BuildTool/README.md) builds modular programs from
`.dkp` projects, with explicit dependencies, optional modules, and native linking.

stages
1. design stage - make sure the syntax is consistent, complete and simple to use
2. c++ bootstrap - write the compiler first in C++ basic lexer/parser, use LLVM
3. write compiler in Duck - use C++ compiler to compile duck compiler so the final compiler is written in duck and does not depend on C++
4. refactor Duck compiler - there will be some functions that are inefficient by the C++ bootstrap and might need to be re-done in assembly.
5. compile optimization - make sure the compiler implements all applicable optimization techniques.
6. feature full compiler - is the syntax feature full, disk operations, memory operations, network operations and graphics.
7. integration for debugging and IDEs - pre-compiler and pdb file emitter might be needed.
8. Something, something advertise Duck - Quack.

# Syntax
*note that this is not final syntax and it is subject to change*

For a guided tutorial with correct patterns and deliberately broken cases, start with [Examples/00_StartHere.dk](Examples/00_StartHere.dk). Each lesson is an independent `.dk` file with explanations in comments. These are design examples; they have not been validated by a Duck compiler. Remaining unspecified edge cases are recorded in [Examples/DesignQuestions.dk](Examples/DesignQuestions.dk).

Duck is **case sensitive**.                  
Duck is using for it's files the **.dk** extension.

Example of a Hello world program in Duck:
``` c++
Main()
{
    Print("Hello world")
}
``` 
## Comments in Duck
Comments are the same as in c# or c++ as there is no need to change proven standards.
``` c++
// inline comment
/*
* multi-line comment
*/
```

## Termination in Duck
Duck is using newline terminated expressions but in some cases ; are allowed.

## Formatting in Duck
Formatting is up to the user and the formatting in samples should not be taken as the law, if you prefer different formatting you can use it as long each expression is ended with new line or the } symbol

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
Duck does not support Automatic type for return or arguments to preserve information for the programmer.
## Access protection in duck
Duck supports public and protected access for container members.

The default protection access is **public**.

Setting Access protection in Duck. We use a non-verbose way to specify what the protection is, as it is not most of the time the focus of programers but more used for the IDE that shows what this object can access. so we use + - operators inspired by UML models as it seems to be the most straightforward way to say what protection is in use.
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

## Function calling in duck
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
each program start from main function that has to be declared **outside of any container**.
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

we can execute instructions just placed outside of containers inside file but we need to call the Exec function on the file as they do not run by default.
``` c++
// inside main.dk
Main(string [] args) {
    Exec("hello.dk")  // <- Path to file
}
// inside hello.dk
    Print("Hello world")
``` 

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

we can also chain static and non-static bodies of container to get object definition and static functions with the same name. This separation will hopefully make logical blocks, that programers can document and collaps as needed.
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

## Varibales in Duck
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
name     // string 1 byte UTF-8
string   // string 2 byte TChar
char
uint
uint8
uint16
uint32
uint64
int8
int16
int32
int64
u1-64   // example u2, 2 byte unsigned integer
i1-64   // example i8, 8 byte signed integer
date
time
datetime
// special types are
big   // <-- that holds arbitrary size of int number --- should be used only for working with very large numbers
ubig
tiny  // <-- holds floating point number of 69 digits  --- should be used only for working with very small numbers to get precision
utiny 
element // <-- single element of a tags tree
markup // <-- hold tags tree for Mark Up  
func // <-- holds function
macro // <-- return type of macros
```
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
- Narrowing `long` to `int` retains the low 32 bits, interpreted as a signed
  integer. Widening `int` to `long` preserves the signed value.
- Floating-point narrowing may round or overflow to infinity. A cast of an
  already-rounded value does not recover precision.
- Numeric-to-`bool` is false for zero and true otherwise (including NaN).
  `bool` converts to numeric `0` or `1`.

The bootstrap supports casts among `int`, `long`, `float`, `double`, and `bool`,
including `int32`/`int64` aliases. Casts involving objects, pointers, text, and
`void` are not implemented. Implicit narrowing remains rejected; write an
explicit cast when conversion is intended.

## Constant variables in Duck
as cont sign we use the same as static
``` c++
#int age  // <- this is constant value, so it can't be changed
```

## Functions in duck
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

Argument refrence is declared with & symbol symular to C++
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
if you want to let the compiler decide what the type is. i have found that var, let, auto or whatever you call this type is only used to tell the compiler that in fact is his job to decide, instead we choose non-verbose way to tell this compiler with : sign.
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
?ex.Foo()   // <-- shorter version of if not null above. will be only executed it ex is valid

// ? operator checks all containers in call so it can be something like
Owner
{
  Example* ex() 
}

Owner* o()
?o.ex.Foo()  // <-- in this case it checks not null for ex and for o containers
```

Both `if ex` followed by `ex.Foo()` and `?ex.Foo()` protect the checked use against null invalidation during the call. The `?ex.Foo()` form executes only if the reference and the function are valid; otherwise it skips the call.
## Heap allocation in Duck
when variable gets inicilized we also allocate the variable. we use C++ style pointer symbol * to specify if it's stack allocation or heap allocated
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

If we want to do inplace heap allocation we can use the :: symbol
``` c++
Example* [] examples
examples += ::Example()
```

## Object references in Duck
In Duck there is only one owner of the value called parent, but this value can still have more references.
``` c++ 
Example
{} // declaration

Example* e = Example()
e2 : e  // <-- holds now reference to e value but it is not it's parent
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
##  Pointers in Duck
Heap allocations are inherently pointer and do have size of 8 bytes

``` c++ 
Example
{}

Example* e = Example() // pointer to allocated Example
```

empty ponter can be done with out calling the construtor
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
e.Bob += 1 // unsafe <-- will actually create memory leek 
e.Rob = null

Print(e.Bob == null) // prints false, as it is no longer tracted it will not get set to null

```


##  Constructors/Destructors in Duck
In Game environment you usually have functions like Start/Spawn, Tick/Update, Destroy or variants on this, that are actually separated from Constructors and Destructors. this relates to the life cycle of this object, where destructor is final and you want to destroy it only at the end of the frame, for start/Spawn you might want to do something before the consturctor is called

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

Duck offers option to deal with Constructor issue, using partial inicialization:
``` c++ 
Example
{
    -Example() // <-- partial constructor that gets called before the regular one, but it can be set after, this constructor gets called regardless of what is called in other constructors and in childrens
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

Duck also let's you call the constructor again to reinicilaze if needed
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
int queue s1            // queue of for int type  - FIFO
int stack s2            // stack of for int type  - LIFO 
string, string pair s3  // <-- in c++ map,  in C# called dictionary, and honestly the best name would be KayPair, but pair must do as it is shorter and clearly says what it does 
```
we can assign values by = and use { } quotes to insert actual values.
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
s -= [1]   // remove value at index 1, this does shifts all values after index 0 by index-1 positions in array
s--        // removes last value in array 
s.Len      // number of elements in array
```
working with pair collection
``` c++
string, string pair s 
s += "name" : "Bob"  // add pair Key: "name", Value: "Bob" 
s["name"] = "Tom"    // change Value of Key "name" to "Tom" 
s++                  // tries to pattern much key (similar to ms excel for example month) if it fails add 1 to last key and sets value to none
s--                  // removes at last added key
s -= ["name"]        // remove pair Key: "name", Value: "Tom"
s.Clear()            // removes all elements in collection
```
if we need to know the index of add or remove we can add your assignment into () brackets then this expression will return an appropriate return type to the expression
``` c++
int [] s = {1,1} 
int index = (s += 4) 
Print(index)  // prints 3
```

## Bounds checking in Duck
if you need to check if value is valid you can use IsValid() function or use shorten if check
``` c++
int [5] s 
if IsValid(s[6]) // this checks bound of the array s
  s[6] = 5       // this will not be executed as 6 is out of bounds
//or
?s[6] = 5        // ? checks bound of the array s and will not be executed as 6 is out of bounds
```
## Multidimensional collections collections in Duck
Duck supports multidimensional collections for multiple use cases 
``` c++
int s1[5][10]  // <-- 2D static array 
int s2[][]     // <-- 2D dynamic array
int s3[-][-]   // <-- 2D dynamic array that has index from int.Min to int.Max. 
               //     Meaning you can access values that are in negative index such as s3[-2][1] this is great for grids as you generally start at 0,0
```

## Collection size prediction in Duck
for Duck compiler to accurately calculate time execution time for async separation, as programmers we can help with that by adding our guesstimate into array declaration
``` c++
int [] s       // <-- dynamic array can hold any number of elements, compiler makes the guess how many elements might contain depending on it's uses in code
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
even thou : is convention in ternary conditional operators, symbol | seams more logical to use as in programming || means logical OR and single | can mean pipe or disjunction, it  makes more sense as representation of if-else split.
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
Duck omits type as for each value must be the same type, Duck allows for sequence declaration with .. operator as it is the most used way to use loops
``` c++
for i : 0...5 // <-- form  0 to 5 Inclusively, it executes loop body 6 times in total
{ 
}
for i : 5...0 // <-- form  5 to 0 inclusively
{
}
```
sequence declaration with .. operator can be used outside of loops
``` c++
n : 0...5  // this will be interpreted as int [] n = {0,1,2,3,4,5}
```
in .. operator we can also use variables to achieve the standard loop pattern "int i = 0; i < loops.Len;  i++"

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
} // <-- this are two nested for loops, the inner for loop has body of {print(n)} 
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
}| // <-- outer loop body is executed before inner loop due to | separator being in behind the body
    i : 0...5 
{
    Print(n)
} 
// Result would be 111111333333   
```
this works only with for loops as i have found you generally want to do nested loops for 2D or 3D arrays or objects within objects.

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
Name.Space.Foo() // it's static part
Name.Space sp()  // it's object part
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
Duck is using the : symbol for declaring inheritance similar to C# or c++. we allow for unlimited container parents separated by , symbol
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
Overriding is not declared, Any protected or public function can be overridden. Vitual pointer gets generated depending if someone has overridden any function.
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
     Walk() // Walk() form Animal
     Eat()
     base.Walk() // Walk() still form Animal
     this.Human.Walk() // Walk() form Human - using this reflection container
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
Duck is using it's reflection system to call functions in runtime, We can call any function on any container by using the .. symbol

``` c++
Example
{
}

Example e()
e..Foo() // Duck call

```
if the function is not pressent in will resoult in runtime error


``` c++
Example
{
}

Example e()
if e..?Foo() // check if Foo is valid
    Print("Foo is valid") // will not print as Foo is not present in Example

?e..Foo() // if function is not present it will not execute

```

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
Duck is using inheritance style declaration for type enum declaration with combination with the @ symbol representing enumeration block and # representing static blocks. This block is also expandible same as #{} or {} blocks.
``` c++
Day : byte 
#@{
    Sunday, Monday, Tuesday, Wednesday, Thursday, Friday, Saturday
}
// more verbose way 
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
  FirstAge: ?Ages[0] 	// checks if index 0 is valid and returns that value now it is 20, if index 0 is not valid it returns null
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
``` c++
uint x = 0x05		// 00000101
Print(x & 0x03) 	// 00000001  <-- Bitwise AND   
Print(x | 0x03)		// 00000111  <-- Bitwise OR
Print(x ^ 0x03) 	// 00000110  <-- Bitwise XOR
Print(~x)		    // 11111100   <-- Bitwise NOT
Print(x << 1)		// 00001010  <-- Left shift 
Print(x >> 1)		// 00000010  <-- Right shift 
``` 
you can also use them in assignment
``` c++
uint x = 0x05
x &= 0x03
Print(x)  	// 00000001 
``` 
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
## Reflection system in Duck
The main reason for reflection system in duck is to allow for the creation of Game Engine visualizations based on the code. the reflection system can be accessed using this keyword and the name of the container.

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
We can add custom data into the reflection system with [] Simular to C# 

``` c++

[args]   // fist we mark enum that we want to be used as part of the custom data
Visibility : byte 
#@{
    Hidden, VisibleNonEdditible, VisibleEdditible
}


Example
{
    [VisibleNonEdditible]
    int Money = 0

    [Hidden]
    AddCash(int money)
    {
        Money += money
    }

    CanWeSeeMoney()
    {
        byte [] tags = this.Example.Money.GetTags()

        if tags.Len > 0 && tags[0] == Visibility.VisibleNonEdditible
        {
            Print("We can see the money")
        }
    }
}

``` 

## Unit tests in Duck
Unit test are part of the core code as Duck will use them it gage correctness and speed while optimizing the compiled code for multithreading, this is sort of to force users of Duck to make test as part of their workflow,
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
        Print(this.Example2.Name + " " + this.Example2.Age) // we get access to the component using this and it has the same name as the container
    }
}


``` 

If we have multiple or uknowen number of childen elemnts we can check them using the GetChildren() function
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
    markup GetGuyComponent(string name, int age)
    {
        return {
            <Guy Name=name Age=age>
            "name age" // we do things differently here from XML values have to be declared as literals or any supported type like string, int and so on, the resoulting value variable will have that type. escaping is done using the \ character
            <Guy/>
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
## Compile time execution in Duck
any function call that have # before it, will get compiled into it's return at compile time (this is also known as baking). It can be very useful for optimalizations.
``` c++
Example 
{
    int Foo(int i) 
    {
        return (2^4)*i + (i > 0)? Foo(i-1) | 0
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
macros in C++ are extremely powerful tool but also completely impossible to debug and read, Duck solves this by functions that gets unwraped at compile time, functions has to return macro type. We force at least some sort of structure by having arguments of the function statically typed.

``` c++
Example 
#{  // Must be in static block

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

## Definitions in Duck
defined constants can be used only and only by the #if, this is to prevent C style defines without type

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
Common program structure in C++ is to have some kind of Start function that allocates stuff on heep, Like loads world that run for the duration of the programs life, or if switched destroyed and re-initialized with new world. The issue is that most objects in the world need to get initialized under the world, this may be true for some that want a reference back to the world but not for all. Or if we have start and stop functionality where we add new objects to the world and want to remove them at the end of play.

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

After Start function we usually have some kind of loop that runs forever. Each cycle of the loop represents a game frame, we might again allocate stuff and the start of the frame and dump them at the end of the frame. if we mark loop using custom scope it will automaticly call the @start and the @end even if we use for example break to escape it, if we call the @end anywhere in the loop it will also escape the loop

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
