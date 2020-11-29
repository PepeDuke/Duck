# Duck
Duck is programming language, focusing on speed and abstraction, without care about compile time. Main feature of using duck is async and sync design is resolved by the compiler with full thread safety.
# Philosophy
there are many languages each great in there own way, so the only reason for making new language is to do something revolutionary. as CPUs have more and more cores over time there is not many reasons to make sigle threaded applications, async programing can be ok but from time to time you run into race conditions, thread overlaps or just additional code to stop your threds from fighting over resources. Duck is here to save the day. As many things are inherently async: some I/O operations, code that takes too long to execute, or any network oprations. Duck solves for all of this in compile time.

No garbage collector, there should not be generated any garbage in the first place. Duck has single parent policie. At witch the owning parent destroys it's children or the parent gets destroyed, the resource gets removed. But until then others can reference this resource allowing for having same resource with multiple references.

Minor features: Some optimalization techniques should be done by the compiler it self, like bool stacking into bit flags. Support for full reflection.

Duck syntax is based on my personal preferences and my imagination of the perfect syntax. i like type safety as in my opinion it makes code simpler to read especialy as you geting return from fucntion/method and you have to mouse over to see what it actualy returning, with type safety you see it right there. But on the other hand i'm not fan of verbose syntax, as it makes the whole code unnecessarily too large.

Duck, as the name implies, also supports Duck typing, but instead of resolving in runtime by the interpreter, it is resolved by the compiler and simply returns error message in runtime if it's printed out or just omitts this execution from final compiled code if function decleration does not exist.

Duck forces type safety for fucntions as thay are the worse offenders in automatic return type or automatic arguments. i'm looking at you JavaScript. this can be bypassed to a certen extend with duck typing.

# Project status
Current status: design stage

stages
1. design stage - make sure the syntax is consistent, complete and simple to use
2. c++ bootstrap - write the compiler first in c++ basic lexer/parser might use lex/Yacc + C++
3. write compiler in Duck - use c++ compiler to compile duck compilator so the final compilator is writen in duck and does not depend on c++
4. refactor Duck compilator - there will be some functions that are inefficient by the c++ bootstrap and might need to be re-done in assembly.
5. compile optimalization - make sure compiler implements all applicable optimization techniques.
6. feature full compiler - is the synatx feature full, disk oprations, memory oprations, network oprations and graphics.
7. integration for debuging and IDEs - pre-compiler and pdb file emiter might be needed.
8. implement IR translation for multiple platforms - this one's a doozy.
9. Something, something advertise Duck - Quack.

# Syntax
*note that this is not final syntax and it is subject to change*

Comments are the same as in c# or c++ as there is no need to change time proven standards.
``` c++
// inline comment
/*
* multi-line comment
*/
```
Duck is case sensitive.                  
Duck is using for it's files the .dk extension.


## Class definitions in Duck
Duck has struct/class combined into one type called container.
``` c++
Example // <-- this is declaration of container
{ 

}
```

Functions/methods in Duck. In Duck we call them functions. The difference from container is thay must use () argument space to be declared. () can't be ommited as it is deterministic factor for functions.
``` c++
Foo() { // <-- this is declaration of function

}
```

If function does not return anything we ommit the void type. But if does return something it needs to specify the type.
``` c++
int Foo() // <-- this is declaration of function with return type
{ 
 return 5;
}
```

So in combination with container we might have something like 
``` c++
Example 
{ 
  int Foo() 
  { 
    return 5;
  }
}
```
funtion argument and return have to be type safe
``` c++
int Foo(int i) <-- argument int i
{
    return i*2;
} 
```
Duck does not support Automatic type for return or arguments as it can lead to confusion, need for TypeOf functions and additional checking in runtime but on the other hand as it suports duck typing with the obj type and it can be utilized to bypass some of the strong type nature of functions.
## Access protection in duck
In real world application access protection is around 50%/50% between public and private members (if we ignore protected), meaning there is no right answer to this age old question what the default protection access should be. But because container are struct/class and we believe all developers are trained professionals.

The default protection access is **public**.

Setting Access protection in Duck. we use non-verbose way to specify what the protection is, as it is not most of the time the focus of programers but more used for the IDE that shows what this object can access. so we use + - operators inspered by UML models as it seams to be the most strait forward way to say what protection is in use.
``` c++
-InternalFoo() // <--  private function
{ 

} 
+-ChildFoo() // <-- protected function // or -+
{   

} 
+Foo() // <-- public function
{

} 
```
The protected sign +- or -+ is different from the UML model's #, as it seems to me that # sign in this case makes no sense at all. Also # in Duck is used differently. The +- has been chosen as protected access is logicly something between public and private.

## Function calling in duck
Duck is object-oriented language and so we can use object call.
``` c++
// declaration
Example 
{  
    Foo()
    {
    
    }
}

Example ex = Example(); // <-- create variable ex that is type Example
ex.Foo();               // <-- calling of function Foo defined in the declaration inside Example
```

We can also shorten the varible decleration for object by using
``` c++
Example ex();           // <-- create variable ex that is type Example, expresion equivalent to Example ex = Example();
```

## Program start in Duck
each program start from main function that has to be declered outide of any container.
``` c++
main (string [] args) {
 Print("Hello world");
}
``` 

we can execute instructions just placed outside of containers inside file but we need to call the Exec function on the file as they do not run by default.
``` c++
// inside main.dk
main (string [] args) {
  Exec("hello.dk");
}
// inside hello.dk
 Print("Hello world");
``` 

## Duck typing in Duck
Duck supports Duck typing so it let's you call functions that are not accessible or do not exist inside the container
``` c++
Example 
{  
   -Foo() // same as before but now Foo is private
   {  
   }
}
Example ex(); 
ex.Foo();  // <-- this call should not work as Foo is private, so what will happen this line simply will be omitted from the compiled code and won't be executed
// but it is not considered as error
```
any container can be casted to base type obj
``` c++
obj ex = Example();
ex.Foo();
```
this can be used to achieve polymorphism for any container no matter what is inheritance status is
``` c++
TryToQuack(obj duck)
{
   duck.Quack();
}

Duck 
{
  Quack(){print("Quack");}
}

Person
{
  Quack(){print("oh yes young chap, i most definitely can quack /kwak/, you see");}
}

Duck duck();
Person person();

TryToQuack(duck);      // it qucked so it is duck
TryToQuack(person);    // it qucked so it also duck
```
There might be cases where you actually want test if function can be called successfully or not. we can check with .?  operator
``` c++
TryToQuack(obj duck)
{
   if duck.?Quack() // this does not call Quack(). It just returns true if duck can Quack, if can't it returns false
      duck.Quack();
   else
      print("this "+ duck +" can't quack");
}
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

Example.Foo(); // <-- static function call
```

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
Example #{}{}#{}#{}{} // <-- still valid code
```

if multiple containers have the same name and parent container thay are considered the same container. 
``` c++
Example // <--  non-static  container
{  

    Foo () // <-- not accessebele by  Example.Foo();
    {

    }
}

Example  // <--  only-static container any variables or fucntions in here are static
#{ // <-- static indicator #

    Foo () // <-- accessebele by  Example.Foo();
    {

    }
}
```
So above and below examples are equivalent
``` c++
Example // <--  non-static  container
{  
    Foo () // <-- not accessebele by  Example.Foo();
    {

    }
}
#{    // <--  only-static container any variables or fucntions in here are static
    Foo () // <-- accessebele by  Example.Foo();
    {

    }
}
```

## Compile time execution in Duck
any function call that have # before it, will get compiled into it's return at compile time (this is also known as baking). It can be very useful for optimalizations.
``` c++
Example 
{
    int Foo (int i) 
    {
            return (2^4)*i + (i > 0)? Foo(i-1) | 0;
    }
    int Foo2()
    {
       int fooResult = #Foo(5); // <-- this gets replaced by whatever Result of this function is at compile time.
       fooResult++;
       return fooResult;
    }
}
```

## Varibales in Duck
all variables are lower case, no uppercase types exist, if in Duck you want to get type Max you do int.Max;
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
// speciall types are
big   // <--- that holds arbitrery size of int number --- should be used only for working with very large numbers
ubig
tiny  // <--- holds floating point number of 69 digits  --- should be used only for working with very small numbers to get precision
utiny 
```
## Constant varibales in Duck
as cont sign we use the same as static
``` c++
#int age; // <- this is constant value, so it can't be chagnged
```

## Automatic type declaration in Duck
if you want to let the compiler decide what the type is. i have found that var, let, auto or whatever you call this type is only used to tell the compiler that infact is his job to decide, insted we cose non-verbose way to tell this compiler with : sign.
``` c++ 
age : 54; // <- this will be compiled as int age = 54;
```
## Null references in Duck
Containers main parent can be forced to destroy resource by setting it to null.
``` c++
Example ex(); 
ex.Foo();     // <--valid call
ex = null;
ex.Foo();     // <-- not valid call, ex container no longer exists
```

we can check if varibale is null by
``` c++
if ex
  ex.Foo();
// or
?ex.Foo();   // <-- shorter version of if not null above. will be only executed it ex is valid

// ? operator checks all containers in call so it can be something like
Parent
{
  Example ex(); 
}

Parent p();
?p.ex.Foo(); // <-- in this case it checks not null for ex and for p containers
```
## object references in Duck
In Duck there is only one Owner of the Value called parent and but this value can still have many more refrences.
``` c++ 
Example{} // decleration

Example e();
Example e2 = e; // <-- holds now reference to e value but it is not it's parent
e = null; 
Print(e2);      // <-- e2 prints out null as the refrenced value is now null 
```

you can pass Value Parenthood with : sign
``` c++ 
Example e();
e2 : e;     // <-- now e2 is main parent of e value
e = null;
Print(e2);  // <-- e2 is still valid
```
in context to varibales : sign is only for declering new variables
``` c++ 
e : 50;
e : 5; // <-- not valid, variable e has already been declared
```
this is mostly to keep consistency between = and : but also to let programers know the importance of the : sign.
*This might be too annoying in the real world, so this restriction might be removed*
##  Collections in Duck
in duck dynamic collection (in C# List, in C++ Vector) and dynamic array are one and the same and compiler decides what is the faster approach, as functionaly are almost equivalent. sumularly to JavaScript or Python.

``` c++ 
int [50] s1;  // <-- static array that holds 50 int values
int [] s2;    // <-- dynamic array can hold any number of elements, might be same as (in C# List, in C++ Vector) or dynamic array
```
Other collection are sepert as they are functionaly different.
``` c++
int queue s1;
int stack s2;
string, string pair s3; // <-- in c++ map,  in C# called dictionary, and honestly the best name would be KayPair, but pair must do as it is shorter and clearly says what it does 
```
we can assign values by = and use { } quotes to insertet actual values.
``` c++
int []  s1 = {5,1,2}; 
int queue s2 = {2,3,4}; // <- 2 is first and 4 last in queue so if we deque we will get 2
int stack s3 = {2,3,4}; // <- 2 is first and 4 last in stack so if we pop we will get 4
string, string pair s4 = {"name" : "Bob", "adress" : "Codesquare 5", "city": "New York"}; 
```
pair has also case where you add only one type 
``` c++
string pair s1 = {name: "Bob", adress : "Codesquare 5"}; 
Print(s1[name]); // <-- prints Bob
```
this is similar to "string, string pair" in use cases but it is marginally faster.

## Automatic type collections in Duck
as with varibales we use.
``` c++
s1 : {5,1,2}; // will be interepreted as int []  s1 = {5,1,2}; 
s2 : {"name" : "Bob", "adress" : "Codesquare 5"} // will be interpreted as pair
```
queue and stack has no automatic type decleration.
## Working with collections in Duck
in Duck we use non-verbose way to work with collections
``` c++
s += 5;    // adds value 5 to array
s++;       // adds value 0 to array
s[1] += 5; // adds 5 to a value in array with index 1
s[0] = 5;  // changes value to be 5
s -= [1];  // remove value at index 1, this does shifts all values after index 0 by i-1 positions in array
s--;       // removes last value in array;
s.Len;   // number of elements in array
```
working with pair collection
``` c++
string, string pair s;
s += "name" : "Bob"; // add pair Key: "name", Value: "Bob" 
s["name"] = "Tom";   // change Value of Key "name" to "Tom" 
s++;                 //  <-- not valid expression as there is no default Key
s--;                 //  <-- not valid expression as there is no default Key
s -= ["name"];       // remove pair Key: "name", Value: "Tom"
s.Clear();           // removes all elements in collection
```
## Bounds checking in Duck
if you need to check if value is valid you can use IsValid() fucntion or use shorten if check
``` c++
int [5] s;
if IsValid(s[6]) // this checks bound of the array s
  s[6] = 5;      // this will not be executed as 6 is out of bounds
//or
?s[6] = 5;       // ? checks bound of the array s and will not be executed as 6 is out of bounds
```
## Multidimensional collections collections in Duck
Duck supports multidimensional collections for multyple use cases 
``` c++
int s1[5][10]; // <-- 2D static arrray 
int s2[][];    // <-- 2D dynamic array
int s3[-][-];  // <-- 2D dynamic array that has index from int.Min to int.Max. 
               //     Meaning you can access values that are in negative index sutch as s3[-2][1] this is great for grids as you generaly start at 0,0
```

## Collection size prediction in Duck
for Duck compiler to acurtlly calclulate time exection time for async separation, as programeres we cen help with that by adding our guesstimate into array decleration
``` c++
int [] s;  <-- dynamic array can hold any number of elements, compiler makes the guess how many elements might contain depending on it's uses in code
int [*] s; <-- still dynamic array, but this says to compiler, expect huge amount of values in this array.
int [50*] s; <--still dynamic array, but this says to compiler, expect arround 50 values +- some values in this array.
```

## If statement in Duck
if else statement work as in any other language.
``` c++
if i < 5 // <-- if statement with multy line body 
{ 
  i++;
}
else
{
 i--;
}


if i > 10 // <-- if statement with single line body
   i--; 
else
   i++;
   
 i = (i == 5)?  0 | 1; // ternary if statement
```
even thou : is convention in ternary conditional operators, symbol | seams more logical to use as in programming || means logical OR and single | can mean pipe or disjunction, it  makes more sense as representation of if-else split.
## Loops in Duck
Duck uses for loop as a for and foreach, simularly to JavaScript
``` c++
int [10] loops;
for int i = 0; i < loops.Len; i++ { // <-- basic for loop
 // loop body that in this case would be executed 10 times
  loops[i].foo();
}
for i : 0; i < loops.Len; i++ { // interpreted as loop above but we use automatic type deleration
  loops[i].foo();
}

for loop : loops {      // <-- for each loop in loops
  loop.foo();
}
for -loop : loops {      // <-- reverse for each loop in loops  -- prefered if removing elements
if loop.PendingDestroy // if some variable is true
  loops -= loop;        // <-- removes this element
}
```
Duck omits type as for each value must be the same type, Duck allows for sequence decleration with .. operator as it is the most used way to use loops
``` c++
for i : 0..5 { // <-- form  0 to 5 Inclusively, it executes loop body 6 times in total
}
for -i : 0..5 { // <-- form  5 to 0 inclusevly
}
```
sequence decleration with .. operator can be used outside of loops
``` c++
n : 0..5  // this will be interepreted as int [] n = {0,1,2,3,4,5};
```
in .. operator we can also use varibales to achive the standart loop pattern "int i = 0; i < loops.Len; i++"

``` c++
for i : 0..loops.Len-1 {
}
```
do-while and while work as in other languages so does keywords sutch as break; continue;
``` c++
while true { // while with body after condition
  Work();
}

do {
  Work();
} while true; // while with body before condition

do {
 if IsWorkDone() == true
    break;
  Work();
}

```

## Loop chaining in Duck 
Duck intreducing non-verbose way to have nested loops, with the | symbol used as loop separator 
``` c++
for n : 0..3 |
    i : 0..5 {print(n);} // <-- this are two nested for loops, the inner for loop has body of {print(n);} 
// Result would be 000000111111222222333333 

// verbose way
for n : 0; n <= 3; n++ { 
  for i : 0; i <= 5; i++ { 
  print(n);
  }
}
```
we can also add body to the outer loop
``` c++
for n : 0..3 |{n++;} // <-- outer loop body is executed after inner loop due to | separator being in front of the body
    i : 0..5 {print(n);} 
// Result would be 000000222222  

for n : 0..3 {n++;}| // <-- outer loop body is executed before inner loop due to | separator being in behind the body
    i : 0..5 {print(n);} 
// Result would be 111111333333   
```
this works only with for loops as i have found you generaly want to do nested loops for 2D or 3D arrays or objects within objects.

## Namespaces in Duck 
Duck has no namespace keyword as it is not needed we can simply use #{} static container as a namespace as it serves the same purpose
``` c++
Name // we create our namespace by creating static container
#{ 
   Space 
   #{

         foo(){}
    }
    {
         foo2(){}
    }       
}
// we can now call Space by
Name.Space.foo(); // it's static part
name.space sp();  // it's object part
sp.foo2();
```
for namespace unwrap we can use the using kayword simular to C# as it is short and works well in large applications
``` c++
/// in diffren file we can add
using Name;

Space.foo() // now we can use Space container as if we are inside Name container;
Space sp();
sp.foo2();
```
Duck supports unrestricted container extensions
``` c++
// file1.dk
Name // extending container Name with foo function
#{  
  foo(){}
}
```
``` c++
// file2.dk
Name // extending container Name with foo2 function that calls function foo that has been declared in file 1
#{  
  foo2(){foo();}
}
```
## Inheritance in Duck 
Duck is using the : symbol for declering inheritance simular to C# or c++. we allow for unlimited container parents separeted by , symbol
``` c++
Animal
{
  Walk(){}
}

Cat : Animal  // Cat inherits Animal's public and protected members
{ 
}

Cat cat();
cat.Walk(); // this calls Walk() decleration in Animal
```
Overriding is not declared, Any function can be overridden, for most part there is not many reason to forbid Overriding, generaly if you override something you call the base fuction anyway and just add something before or after the base functionality.
``` c++
Animal
{
  Walk(){}
}

Cat : Animal // Cat inherits Animal's public and protected members
{ 
  Walk(){base.Walk();} // this overrides Walk() decleration in Animal
}
Cat cat();
cat.Walk(); // this calls Walk() decleration in Cat
```
multyple perents
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
     Walk();
     Eat();
     Walk();
  }
}
```
in some cases prent functions can become inaccessible form child container. this is middle ground between not suporting multyple inheretence and fully suporting it. multiple inheritance gives level of freedom that we want, but we don't not recommended overlapping parents.
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
Cat : Animal, Human  // in this case priority is from left to right. if function is alerdy defined it simply can't be re-defined in a second perent.
{
  HaveADinner()
  {
     Walk(); // Walk() form Animal
     Eat();
     base.Walk(); // Walk() still form Animal
     // in this case Walk() forom Human simply can't be acessed 
  }
}
```
if extending container that has inheritance it must be declared in all files to keep visible the fact that it is inheriting something. in this case it should be IDE's work to keep this fact consistant and auto fill to all files if changed.
``` c++
// file1.dk
Cat : Animal 
{
}
```
``` c++
// file2.dk
Cat   // <-- invalid, this must be "Cat : Animal" also, We expect IDE to solve this using auto refactor options
{ 
}
```
## Interfaces  in Duck 
Duck is using Duck typing and therefore there is no need for interfaces. But if we want to expose set of functions to be used by user, we can make function definition container.
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
    interface.Foo();
   }
}

SomethingElse {}      // to show that inheretence still works
Duck : SomethingElse, PredefinedFunctions
{
   Foo(){Print("actual implementation");} 
   Foo2(){Print("actual implementation");}
}

Duck d;
WantToUseFunctions.UseFunctions(d);
```

## Enums in Duck
Duck is using inheritance style declaration for type enum declaration with combination with the @ symbol representing enumeration block and # representing static block. This block is also expandible same as #{} or {} blocks.
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

Print(Day.Sunday); // prints Sunday
Print(Day.Sunday == 0); // prints true  <-- direct cast

```
in Duck you can also make enums out of non-tradicinal values sutch as floats and strings
``` c++
IrationalNumbers : float
#@{
 Pi: 3.141592653589,
 SqRoot2: 1.4142135623, 
}

Incemets : float #@{
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

Print(CommonNames.Tom); // prints Tom
Print(CommonNames.Bob + " and " + CommonNames.Tom); // prints Bob and Thomas <-- direct cast  
```

## Properties in Duck
the @ symbol is used as representation to symalar to Get, with inclusion of any non-static varible in @ block, by default anything after : is returned
``` c++
Person 
{
      -int age = 20; // private varibale age
}
@{ // Get export for age // non-static block
  Age: age              
}

Person p;
Print(p.Age);      // prints 20 
Print(Person.Age); // prints Person has no propery Age
// static block for age has not been defined
```
this also allows expresions
``` c++
Population 
{
      -ages : {20,50,12,23,40}; // private array ages
}
@{
  MaxAge: ages.Max, // returns 50
  FirstAge: ?ages[0] // checks if index 0 is valid and returns that value now it is 20, if index 0 is not valid it returns null
}
```
Setter are similarly set inside the ={} block by
``` c++
Person 
{
  -int age = 20; // private varibale age
}
={
  Age: age = value // Set age variable with value on the right handside of =
}

Person p;
p.Age = 25;
Print(p.Age);     // prints 25 
``` 


