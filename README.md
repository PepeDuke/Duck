# Duck
Duck is a programming language, focusing on speed and abstraction, designed mainly for game development. Duck is statically typed and compiled language. Main feature of using Duck is async and sync design is resolved by the compiler with full thread safety.
# Philosophy
There are many languages, each great in their own way, the only reason nowadays for making a new language is to do something revolutionary. As CPUs have more and more cores over time there are not many reasons to make single threaded applications, async programming can be ok but from time to time you run into race conditions, concurrency problems or just additional code to stop your threads from fighting over resources. Duck is here to save the day. As many things are inherently async: some I/O operations, code that takes too long to execute, or any network operations. Duck solves for all of this in compile time.

No garbage collector, there should not be generated any garbage in the first place. Duck has a single parent policy. At which the owning parent destroys its children or the parent gets destroyed, the resource gets removed. But until then others can reference this resource allowing for having the same resource with multiple references.

Support for reflection that can be then used while building game engine

Build in markup tags for building UI/UX

Testing included as core part of the language

Basic data collection types like strings/arrays/maps and so on, are core part of the language. This is firstly that we don't have to create our own or use something like std:: in C++. Secondly this helps duck create a multithreading specific for collections.

Duck forces type safety for functions as they are the worse offenders in automatic return type or automatic arguments. I'm looking at you JavaScript.

# Why this syntax?
Duck syntax is based on my personal preferences and my imagination of the perfect syntax. I like type safety as in my opinion it makes code simpler to read especially as you get return from functions/method and you have to mouse over to see what it is actually returning, with type safety you see it right there. But on the other hand i'm not fan of verbose syntax, as it makes the whole code unnecessarily too large.

# Project status
Current status: **design stage**

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

Duck is **case sensitive**.                  
Duck is using for it's files the **.dk** extension.

Example of a Hello world program in Duck:
``` c++
Main ()
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

## termination in Duck
Duck is using newline terminated expressions but in some cases ; are allowed.

## Class definitions in Duck
Duck has a struct/class combined into one type called container.
``` c++
Example // <-- this is declaration of container
{ 

}
```

Functions/methods in Duck. In Duck we call them functions. The difference from containers is they must use () argument space to be declared. () can't be omitted as it is a deterministic factor for functions.
``` c++
Foo()  // <-- this is declaration of function
{

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
In real world application access protection is around 50%/50% between public and private members (if we ignore protected), meaning there is no right answer to this age old question of what the default protection access should be. But because containers are struct/class and we believe all developers are trained professionals.

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
Duck does not have private members instead we use just protected, although C++ discourages the use of protected as it implies the use of inheritance might be true for C++ but not for Duck. while inheriting the common practice is to read the functionality, so there is not much of a reason to prevent the modification of the "internal state"

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
Main (string [] args) 
{
 Print("Hello world")
}
``` 
*or*
``` c++
Main () 
{
 Print("Hello world")
}
``` 

we can execute instructions just placed outside of containers inside file but we need to call the Exec function on the file as they do not run by default.
``` c++
// inside main.dk
Main (string [] args) {
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
   Foo(){ // <-- static function
   
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

    Foo () // <-- not accessible by  Example.Foo() 
    {

    }
}

Example  // <--  only-static container any variables or functions in here are static
#{ // <-- static indicator #

    Foo () // <-- accessible by Example.Foo() 
    {

    }
}
```
So above and below examples are equivalent
``` c++
Example // <--  non-static  container
{  
    Foo () // <-- not accessible by Example.Foo() 
    {

    }
}
#{    // <--  only-static container any variables or functions in here are static
    Foo () // <-- accessible by Example.Foo() 
    {

    }
}
```

## Varibales in Duck
all variable types are lower case, no uppercase types exist, if in Duck you want to get type Max you do int.Max 
List of allowed types:
``` c++
bool
byte
short
int
long
float
double
string
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
date
time
datetime
// special types are
big   // <--- that holds arbitrary size of int number --- should be used only for working with very large numbers
ubig
tiny  // <--- holds floating point number of 69 digits  --- should be used only for working with very small numbers to get precision
utiny 
markup // <--- hold tags tree for Mark Up  
func // <-- holds function
```
## Constant variables in Duck
as cont sign we use the same as static
``` c++
#int age  // <- this is constant value, so it can't be changed
```

## Compile time execution in Duck
any function call that have # before it, will get compiled into it's return at compile time (this is also known as baking). It can be very useful for optimalizations.
``` c++
Example 
{
    int Foo (int i) 
    {
            return (2^4)*i + (i > 0)? Foo(i-1) | 0
    }
    int Foo2 ()
    {
       int fooResult = #Foo(5) // <-- this gets replaced by whatever Result of this function is at compile time.
       fooResult++
       return fooResult
    }
}
```

## Automatic type declaration in Duck
if you want to let the compiler decide what the type is. i have found that var, let, auto or whatever you call this type is only used to tell the compiler that in fact is his job to decide, instead we choose non-verbose way to tell this compiler with : sign.
``` c++ 
age : 54  // <- this will be compiled as int age = 54 
```
## Null references in Duck
Containers main parent can be forced to destroy resource by setting it to null.
``` c++
Example ex() 
ex.Foo()      // <--valid call
ex = null
ex.Foo()      // <-- not valid call, ex container no longer exists
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
  Example ex() 
}

Owner o()
?o.ex.Foo()  // <-- in this case it checks not null for ex and for o containers
```
## Heap allocation in Duck

``` c++ 
Example
{
    int val

    Foo()
    {
    }
} 

// Stack allocation
Example e = Example()
Example e2()

// Heap allocation
Example e : Example()
e2: Example()

// Stack allocation
int num = 5
num2 : 5

// Heap allocation
int num : int
num2 : int

// Stack allocation
Example e3 = e2
Example e4 = {val : 5}

// Heap allocation
Example e4 : {val : 5}

// Depends on the allocation of e2
e3 : e2

```
For non-container values the Heap allocation is somewhat more difficult to use, we assume that these types are usually Stack allocated.

as to have a Heap allocated int you have to do
``` c++ 
num2 : int 
num2 = 5  // <- can't be done in single line
```

## Object references in Duck
In Duck there is only one owner of the value called parent, but this value can still have more references.
``` c++ 
Example
{} // declaration

Example e()
Example e2 = e  // <-- holds now reference to e value but it is not it's parent
e = null 
Print(e2)       // <-- e2 prints out null as the referenced value is now null 
```

you can pass value parenthood with :: symbol
``` c++ 
Example e()
e2 :: e      // <-- now e2 is main parent of e value
e = null
Print(e2)   // <-- e2 is still valid
```

if main parent is local variable that gets returned it passes as main parent reference giving you the ability to create factory functions

``` c++ 
Cake
{
   int slices

   Eat()
   {
     if(slices <= 0)
        Print("cake has been eaten")
     else
        slices--
   }
}

CakeFactory
@{
       Cake MakeCake()
       {
           Cake c()
           c.slices = 2
           return c
       }
}

cake : CakeFactory.MakeCake()
cake.Eat()
cake.Eat()
cake.Eat()  // cake has been eaten
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
    -Example() // <-- partial constructor that gets called before the regular one, but it can be set after, this constructor gets called regardles of what is called in other constructors and in childrens, to complite 
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
Gun
{
    -Gun(World world) // <-- partial constructor -- possibly in parent game object/entity
    {
        Print("Hello")
    }

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
collections may be static allocated on stack or dynamic allocated on heap.

``` c++ 
int [50] s1  // <-- static array that holds 50 int values
int [] s2    // <-- dynamic array can hold any number of elements, might be same as (in C# List, in C++ std::vector) or dynamic array
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
as with variables we use.
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
int [] s   <-- dynamic array can hold any number of elements, compiler makes the guess how many elements might contain depending on it's uses in code
int [*] s  <-- still dynamic array, but this tells the compiler to expect a huge amount of values in this array.
int [50*] s  <--still dynamic array, but this says to the compiler, expect around 50 values +- some values in this array.
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
for i : 0..5 // <-- form  0 to 5 Inclusively, it executes loop body 6 times in total
{ 
}
for i : 5..0 // <-- form  5 to 0 inclusively
{
}
```
sequence declaration with .. operator can be used outside of loops
``` c++
n : 0..5  // this will be interpreted as int [] n = {0,1,2,3,4,5}
```
in .. operator we can also use variables to achieve the standard loop pattern "int i = 0; i < loops.Len;  i++"

``` c++
for i : 0..loops.Len-1 
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
for n : 0..3 |
    i : 0..5 
{
Print(n)
} // <-- this are two nested for loops, the inner for loop has body of {print(n)} 
// Result would be 000000111111222222333333 

// verbose way
for n : 0; n <= 3; n++ { 
  for i : 0; i <= 5; i++ { 
    Print(n)
  }
}
```
we can also add body to the outer loop
``` c++
for n : 0..3 |
{ // <-- outer loop body is executed after inner loop due to | separator being in front of the body
n++
} 
    i : 0..5 
{
Print(n)
} 
// Result would be 000000222222  

for n : 0..3 
{
n++
}| // <-- outer loop body is executed before inner loop due to | separator being in behind the body
    i : 0..5 
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
## Interfaces  in Duck 
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

Duck d
WantToUseFunctions.UseFunctions(d)
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

Increments : float #@{
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
  -int age = 20 // private variable age
}
@{ // Get export for age // non-static block
  Age: age              
}

Person p
Print(p.Age)      // prints 20 
Print(Person.age) // prints Person has no property Age
// static block for age has not been defined
```
this also allows expressions
``` c++
Population 
{
      -ages : {20,50,12,23,40}  // private array ages
}
@{
  MaxAge: ages.Max, // returns 50
  FirstAge: ?ages[0] 	// checks if index 0 is valid and returns that value now it is 20, if index 0 is not valid it returns null
}
```
Setter are similarly set inside the ={} block by
``` c++
Person 
{
  -int age = 20 // private variable age
}
={
  Age: age = value // Set age variable with value on the right hand side of =
}

Person p
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
Print(~x)		// 11111100   <-- Bitwise NOT
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
lambda
{
   // standard declaration
   int foo ()
   {
     return 5
   }

   // Lambda function with return
   func f =  @ int (){
    return 5
   }
   
   // Lambda function takes in references to resources from outside if they are needed.
   func f2 = @ (){
     Print(f()) 
   }
}

lambda l
l.f2()  // prints 5
``` 
## Threading control in Duck
threading in Duck flows rules of eventual correctness but in some cases you need to force in order execution. we can declare scope that has a $ prefix to force in order execution. The dollar symbol makes sort of sense as the application that needs in order execution the most is banking.
``` c++
Bank
{
    int cash = 5
   
    AddCash(int amount)
    {
      cash += amount
    }
    
    int Withdraw (int amount)
    {
       if cash - amount >= 0
       {
           cash -= amount
           return cash
       }
       else
       {
          int amountLeft = cash
          cash = 0
          return amountLeft
       }    
    }
    
    TransferMoneyToBank(int amount, Bank bank )
    {
       gotAmount : Withdraw(amount)
       bank.AddCash(gotAmount)
    }
    
    Foo()
    {
       Bank A
       Bank B
       
       ${ // <-- in order execution scope
          A.TransferMoneyToBank(5,B) 
          B.TransferMoneyToBank(10,A)  //<-- in order to end up with Bank A having cash = 10 we might need in order execution
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
        Print(this.Example.GetName()+ "->" + this.Example.Foo.GetName()+"()"); 
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
            Print("We can see the money");
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
        return 3;
    }
}
#{
    int Foo2()
    {
        return 5;
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
            "name age" // we do things differently here from XML values have to be declared as literals or any supported type like string, int and so on, the resoulting value variable will have that type. escaping is done using the \ 
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
