Metamethods let values that carry a prototype (objects, arrays and
resources) customize how the interpreter handles them: a metamethod is a
regular function stored under a reserved dunder name on a prototype, invoked
automatically when the corresponding operation cannot be completed the normal
way. The [manual chapter on prototypes and
metamethods](https://ucode-lang.org/manual/) covers the fallback semantics,
resolution rules and all edge cases in detail; the examples below collect the
common patterns.

ucode supports five metamethods:

| Metamethod     | Operation customized                       |
|----------------|--------------------------------------------|
| `__call__`     | calling the value as a function            |
| `__get__`      | reading a property that is not found       |
| `__set__`      | writing a property that is not an own key  |
| `__delete__`   | deleting a property that is not an own key |
| `__tostring__` | rendering the value as a string            |

```ucode
let foo = proto({}, {
    __call__(...args) { return "called with " + args; },
    __get__(key)      { return key + " is virtual"; },
    __set__(key, val) { rawset(this, key, val); },
    __delete__(key)   { return rawdelete(this, key); },
    __tostring__()    { return "<my-obj>"; },
});

foo(1, 2);        // "called with [ 1, 2 ]"
foo.missing;      // "missing is virtual"
foo.other = 42;   // routed to __set__
delete foo.ghost; // not an own key, routed to __delete__
print(foo);       // <my-obj>
```

## Virtual properties

`__get__` synthesizes properties that are stored nowhere; returning `null`
means "still missing":

```ucode
let o = proto({}, {
    __get__(key) {
        return key == "bar" ? "virtual:bar" : null;
    }
});

o.bar;   // "virtual:bar"
o.foo;   // null
```

Existing properties, own or inherited, always win over the metamethod.

## Delegating to an object

A `__get__` slot may hold an object instead of a function; a lookup that
reaches it re-dispatches on that object with the same key, like Lua's
`__index`. This shares a set of defaults without copying them:

```ucode
let defaults = { host: "0.0.0.0", port: 80 };
let o = proto({}, { __get__: defaults });

o.host;   // "0.0.0.0"
o.port;   // 80
o.nope;   // null, not found there either
```

## Backing stores with raw accessors

Re-entering the customized operation from inside the metamethod re-dispatches
itself into infinite recursion; the escape hatch is `rawget()`, `rawset()`
and `rawdelete()`, which perform the underlying access without dispatch.
Kept on the instance itself, they give the backing-store pattern:

```ucode
let o = proto({}, {
    __get__(key) {
        let v = rawget(this, "_" + key);
        return v === null ? "computed:" + key : v;
    },
    __set__(key, val) {
        rawset(this, "_" + key, val * 10);
    },
    __delete__(key) {
        return rawdelete(this, "_" + key);
    }
});

o.n = 4;
o.n;           // 40
o.other;       // "computed:other"
delete o.n;    // true
```

## Arrays with named fields

Array indices are raw: a key which is a valid index never dispatches a
metamethod. Named keys do, and since arrays have no own-key storage, the
values must be routed to other storage, here a plain object captured by
closure:

```ucode
let store = {};

let a = proto([ 1, 2 ], {
    __set__(key, val) { store[key] = val; },
    __get__(key) { return store[key]; },
    __delete__(key) { return delete store[key]; }
});

a.tag = "lan";     // __set__
a.tag;             // "lan", __get__
delete a.tag;      // true, __delete__
a[2];              // null, raw index read, no __get__
```

Without a `__set__`, a non-index write on an array is silently dropped in
non-strict mode and raises a type error in strict mode.

## Callable values

`__call__` makes a value invocable; `this` is the value being called, and
callable values work anywhere a function is accepted, including as callbacks
for builtins:

```ucode
let add1 = proto({}, {
    __call__(x) { return x + 1; }
});

add1(41);                // 42
map([ 1, 2, 3 ], add1);  // [ 2, 3, 4 ]
```
