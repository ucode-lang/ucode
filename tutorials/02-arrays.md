The [manual chapter on arrays](https://ucode-lang.org/manual/) covers the
value type, indexing semantics and every builtin in detail. The examples below
collect implementations of frequently needed operations that are not part of
the core library, plus one pitfall worth knowing.

## Mapping builtin functions

`map()` (and `filter()`, `sort()`, ...) invoke their callback with the
element, its index and the array itself. Passing a builtin that takes
optional arguments directly therefore feeds it unintended values:

```ucode
let strings = [ "10", "32", "13" ];

map(strings, int);   // [ 10, "NaN", 1 ] - the index is read as the base!

map(strings, (x) => int(x));   // [ 10, 32, 13 ]
```

Wrap such builtins in an arrow function whenever they take more than one
argument with defaults, e.g. `int()`, `substr()`, `match()`.

## Set operations

The `in` operator makes membership testing concise:

```ucode
function intersect(...arrays) {
    if (!length(arrays))
        return [];

    let result = arrays[0];

    for (let i = 1; i < length(arrays); i++)
        result = filter(result, (item) => item in arrays[i]);

    return uniq(result);
}

intersect([ 1, 2, 3, 4 ], [ 2, 3, 5 ], [ 2, 3, 6 ]);   // [ 2, 3 ]
```

```ucode
function difference(array, ...others) {
    return filter(array, (item) => {
        for (let other in others)
            if (item in other)
                return false;

        return true;
    });
}

difference([ 1, 2, 3, 4, 5 ], [ 2, 3 ], [ 4 ]);   // [ 1, 5 ]
```

## Merging, chunking, summing, flattening

The variadic `push()` combined with the spread operator merges arrays:

```ucode
function merge(...arrays) {
    let result = [];

    for (let arr in arrays)
        push(result, ...arr);

    return result;
}

merge([ 1, 2 ], [ 3, 4 ], [ 5, 6 ]);   // [ 1, 2, 3, 4, 5, 6 ]
```

```ucode
function chunk(array, size) {
    let result = [];

    for (let i = 0; i < length(array); i += size)
        push(result, slice(array, i, i + size));

    return result;
}

chunk([ 1, 2, 3, 4, 5, 6, 7, 8 ], 3);   // [ [ 1, 2, 3 ], [ 4, 5, 6 ], [ 7, 8 ] ]
```

```ucode
function sum(array) {
    let result = 0;

    for (let item in array)
        if (type(item) == "int" || type(item) == "double")
            result += item;

    return result;
}

sum([ 1, 2, 3, 4, 5 ]);   // 15
```

```ucode
function flatten(array, depth) {
    depth ??= 1;

    let result = [];

    for (let item in array) {
        if (type(item) == "array" && depth > 0) {
            for (let sub in flatten(item, depth - 1))
                push(result, sub);
        } else
            push(result, item);
    }

    return result;
}

flatten([ 1, [ 2, [ 3, 4 ], 5 ], 6 ]);     // [ 1, 2, [ 3, 4 ], 5, 6 ]
flatten([ 1, [ 2, [ 3, 4 ], 5 ], 6 ], 2);  // [ 1, 2, 3, 4, 5, 6 ]
```

## Deep copying

Arrays and objects are reference types, so copies must be made explicitly;
note the forward declaration, as ucode does not hoist function declarations:

```ucode
function deepCopyObject;

function deepCopy(arr) {
    if (type(arr) != "array")
        return arr;

    let result = [];

    for (let item in arr) {
        if (type(item) == "array")
            push(result, deepCopy(item));
        else if (type(item) == "object")
            push(result, deepCopyObject(item));
        else
            push(result, item);
    }

    return result;
}

function deepCopyObject(obj) {
    if (type(obj) != "object")
        return obj;

    let result = {};

    for (let key in keys(obj)) {
        if (type(obj[key]) == "array")
            result[key] = deepCopy(obj[key]);
        else if (type(obj[key]) == "object")
            result[key] = deepCopyObject(obj[key]);
        else
            result[key] = obj[key];
    }

    return result;
}
```

The object half of this pair is re-used by the
[deep merge recipe](tutorial-03-objects.html).
