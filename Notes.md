# Notes on Unfamiliar Syntactic Constructs

# `asm volatile(<assembly code>)`, and `volatile` in general
The `asm` keyword denotes inline assembly code -- a C++ functionality. Using `volatile` to quantify `asm` disables some compiler optimization such that this `asm` is guaranteed to run / be assembled within the `out` file. 

In general, `volatile` denotes that the type is NOT GUARANTEED to be deterministically changed solely by code IN CONTEXT (i.e., due to modification as written within the code to be compiled). In other words, a `volatile` value may change between different accesses even if the program does not proactively make change to the value. 

> Obv. this is very unsafe in Rust, and same goes for other programming languages...

## Implications
Prevent compiler re-use of stale values (RAR which may be optimized such that the memory location to be read from is equivalently copied from the variable to, say, a register. Refer to Rustonomicon Ch. 3?).

When used in conjunction with `asm`, this ensures that the `asm` inline assembly will always be run as expected from programmer POV. 

# `const`, `mutable`, and the **cv type qualifiers**
The `const` keyword does what you assume -- qualifies the immutability of an object (unless on `mutable` fields, which provides interior mutability on `const`-declared objects): 

```c++
struct X {
    mutable int p; 
    const int q; 
    int r; 
}; 

struct Y {
    mutable const void* p; 
    /*
    Parse this as follows: 

         mutable                           (const void) * p
        |interior mutability even at const|type--------|which is a pointer|
    
    As in, `p` is a `mutable` pointer to a `const void` (which, ofc since void, 
    is only allowed as a pointer). 

    For any other uses of `mutable const` this does not make sense. 
    "Mutable const int" wdym by that. Make up your mind dangit. 
    */
    const void* q; 
}; 

int main() {
    const struct X cx = {1, 1, 1}; 
    cx.p = 2; 
    // cx.q = 2; => Disallowed
    // cx.r = 2; => Disallowed

    const struct Y cy = {&cx.q, &cx.p}; 
    cy.p = &cx.p; // `const &int` casted to `const void*`
    // cy.q = &cx.p; => Disallowed

    cx.p = 3; 
    // UNDEFINED BEHAVIOR: `*cy.q` is now 3 instead of 2, as expected from `const`-ness
    
    return 0; 
}
```

> **UNDEFINED BEHAVIOR**
> Indirectly modifying a `const` object through a non-`const` reference/pointer/`mutable` (as shown above) passes static analysis and results in undefined behavior. 

You can also use `const volatile` to qualify an object that is, well, both `const` (immutable) and `volatile` (will mutate spontaneously, cannot be optimized via RAR analysis). 

> This looks a lot like above undef behavior through... 

## `const` functions, and non-static member functions with cv-qualifiers
Functions with different cv-qualifiers have different types and *overload* each other. In general, a non-static member function with a given cv-qualifier sequence applies that cv-qualifier sequence to `*this` (recall `this` in C++ is a pointer). 

# `override`
The `override` keyword specifies that a *virtual function* **must** override a virtual function from its base class. 

> A *virtual function* is a dynamically dispatched function. Also recall that *override* refers to functions of the same **type** (name + argument types + return types). 