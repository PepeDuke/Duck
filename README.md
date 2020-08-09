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

Class definitions in Duck. Duck has struct/class combined into one type called container.
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

Access protection in duck. In real world application access protection is around 50%/50% between public and private members (if we ignore protected), meaning there is no right answer to this age old question what the default protection access should be. But because container are struct/class and we believe all developers are trained professionals.

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

Function calling in duck. Duck is object-oriented language and so we can use object call.
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

Duck typing in Duck. Duck supports Duck typing so it let's you call functions that are not accessible or do not exist inside the container
``` c++
Example {  
   -Foo(){  // same as before but now Foo is private
   }
}
Example ex(); 
ex.Foo();  // <-- this call should not work as Foo is private, so what will happen this line simply will be omitted from the compiled code and won't be executed
// but it is not considered as error
```
