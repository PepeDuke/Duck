# Duck
Duck is programming language, focusing on speed and abstraction, without care about compile time. Main feature of using duck is async and sync design is resolved by the compiler with full thread safety.
# Philosophy
there are many languages each great in there own way, so the only reason for making new language is to do something revolutionary. as CPUs have more and more cores over time there is not many reasons to make sigle threaded applications, async programing can be ok but from time to time you run into race conditions, thread overlaps or just additional code to stop your threds from fighting over resources. Duck is here to save the day. As many things are inherently async: some I/O operations, code that takes too long to execute, or any network oprations. Duck solves for all of this in compile time.

No garbage collector, there should not be generated any garbage in the first place. Duck has single parent policie. At witch the owning parent destroys it's children or the parent gets destroyed, the resource gets removed. But until then others can reference this resource allowing for having same resource with multiple references.

Minor features: Some optimalization techniques should be done by the compiler it self, like bool stacking into bit flags. Support for full reflection.

Duck syntax is based on my personal preferences and my imagination of the perfect syntax. i like type safety as in my opinion it makes code simpler to read especialy as you geting return from fucntion/method and you have to mouse over to see what it actualy returning, with type safety you see it right there. But on the other hand i'm not fan of verbose syntax, as it makes the whole code unnecessarily too large. Duck, as the name implies, also supports Duck typing, but instead of resolving in runtime by the interpreter, it is resolved by the compiler and simply returns error message in runtime or just omitts this execution from final compiled code.

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
