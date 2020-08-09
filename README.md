# Duck
Duck is programming language, focusing on speed and abstraction, without care about compile time. Main feature of using duck is async and sync design is resolved by the compiler with full thread safety.
# Philosophy
there are many languages each great in there own way, so the only reason for making new language is to do something revolutionary. as CPUs have more and more cores over time there is not many reasons to make sigle threaded applications, async programing can be ok but from time to time you run into race conditions, thread overlaps or just additional code to stop your threds from fighting over resources. Duck is here to save the day. As many things are inherently async: some I/O operations, code that takes too long to execute, or any network oprations. Duck solves for all of this in compile time.

No garbage collector, there should not be generated any garbage in the first place. Duck has single parent policie. At witch the owning parent destroys it's children or the parent gets destroyed, the resource gets removed. But until then others can reference this resource allowing for having same resource with multiple references.

Minor features: Some optimalization techniques should be done by the compiler it self, like bool stacking into bit flags. Support for full reflection.

Duck syntax is based on my personal preferences and my imagination of the perfect syntax. i like type safety as in my opinion it makes code simpler to read especialy as you geting return from fucntion/method and you have to mouse over to see what it actualy returning, with type safety you see it right there. But on the other hand i'm not fan of verbose syntax, as it makes the whole code unnecessarily too large. Duck, as the name implies, also supports Duck typing, but instead of resolving in runtime by the interpreter, it is resolved by the compiler and simply returns error message in runtime if it's printed out or just omitts this execution from final compiled code.

# Project status
Current status: design stage

stages
1. design stage - make sure the syntax is consistent, complete and simple to use
2. c++ bootstrap - write the compiler first in c++ basic lexer/parser might use lex/Yacc + C++
3. write compiler in Duck - use c++ compiler to compile duck compilator so the final compilator is writen in duck and does not depend on c++
4. refactor Duck compilator - there will be some functions that are inefficient by the c++ bootstrap and might need to be re-done in assembly.
5. feature full compiler - it the synatx is feature full and it works as it should.
6. integration for debuging and IDEs - pre-compiler and pdb file emiter might be needed.
7. Something, something advertise Duck

# Syntax
*note that this is not final syntax and it is subject to change*

Comments are the same as in c# or c++ as there is no need to change time proven standards.
``` c++
// inline comment
/*
* multi-line comment
*/
```
Duck is case sensitive

## Class definitions in Duck.
Duck has struct/class combined into one type called container.
``` c++
Example { // <-- this is declaration of container

}
```

Functions/methods in Duck. In Duck we call them functions. The difference from container is thay must use () argument space to be declared.
``` c++
Foo() { // <-- this is declaration of function

}
```

If function does not return anything we ommit the void type. But if does return something it needs to specify the type.
``` c++
int Foo() { // <-- this is declaration of function with return type
 return 5;
}
```

So in combination with container we might have something like 
``` c++
Example { 
  int Foo() { 
    return 5;
  }
}
```

## Access protection in duck
In real world application access protection is around 50%/50% between public and private members (if we ignore protected), meaning there is no right answer to this age old question what the default protection access should be. But because container are struct/class and we believe all developers are trained professionals.

The default protection access is **public**.

Setting Access protection in Duck. we use non-verbose way to specify what the protection is, as it is not most of the time the focus of programers but more used for the IDE that shows what this object can access. so we use + - operators inspered by UML models as it seams to be the most strait forward way to say what protection is in use.
``` c++
-InternalFoo(){  // <--  private function

} 
+-ChildFoo(){   // <-- protected function // or -+

} 
+Foo(){         // <-- public function

} 
```
The protected sign +- or -+ is different from the UML model's #, as it seems to me that # sign in this case makes no sense at all. Also # in Duck is used differently. The +- has been chosen as protected access is logicly something between public and private.

## Function calling in duck
Duck is object-oriented language and so we can use object call.
``` c++
// declaration
Example {  
    Foo(){
    
    }
}

Example ex = Example(); // <-- create variable ex that is type Example
ex.Foo();               // <-- calling of function Foo defined in the declaration inside Example
```

We can also shorten the varible decleration for object by using
``` c++
Example ex();           // <-- create variable ex that is type Example, expresion equivalent to Example ex = Example();
```

## Duck typing in Duck
Duck supports Duck typing so it let's you call functions that are not accessible or do not exist inside the container
``` c++
Example {  
   -Foo(){  // same as before but now Foo is private
   }
}
Example ex(); 
ex.Foo();  // <-- this call should not work as Foo is private, so what will happen this line simply will be omitted from the compiled code and won't be executed
// but it is not considered as error
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

// this checks all containers in call so it can be something like
Parent{
  Example ex(); 
}

Parent p();
?p.ex.Foo(); // <-- in this case it checks not null for ex and for p containers
```


## Static calls in Duck
We have chosen # to represent static meanings depending on context. we can define static Container by # before its body
``` c++
Example #{
   // everything in here is static
}
```
and we can call static function by
``` c++
Example #{
   Foo(){ // <-- static function
   
   }
}

Example.Foo(); // <-- static function call
```

we can also chain static and non-static bodies of container to get object definition and static functions with the same name but this separation will hopefully make logical blocks more visible
``` c++
Example #{
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
Example {  // <--  non-static  container

    Foo () // <-- not accessebele by  Example.Foo();
    {

    }
}

Example #{ // <--  only-static container any variables or fucntions in here are static

    Foo () // <-- accessebele by  Example.Foo();
    {

    }
}
```
So above and below example is equivalent
``` c++
Example {  // <--  non-static  container

    Foo () // <-- not accessebele by  Example.Foo();
    {

    }
}
#{ // <--  only-static container any variables or fucntions in here are static
    Foo () // <-- accessebele by  Example.Foo();
    {

    }
}
```

## Compile time execution in Duck
any function call that have # before it, will get compiled into it's return at compile time (this is also known as baking). It can be very useful for optimalizations.
``` c++
Example {
    int Foo (int i) 
    {
            return (2^4)*i + (i > 0)? Foo(i-1) : 0;
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
all variables are lower case, no uppercase types exist, if in Duck you want to get type Max you do int.Max();
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

## Automatic type declaration in Duck
if you want to let the compiler decide what the type is. i have found that var, let, auto or whatever you call this type is only used to tell the compiler that infact is his job to decide, insted we cose non-verbose way to tell this compiler with : sign.
``` c++ 
age : 54; // <- this will be compiled as int age = 54;
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


