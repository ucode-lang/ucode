The [manual chapter on objects](https://ucode-lang.org/manual/) covers the
value type, key handling and every builtin in detail. The examples below
collect recipes for frequently needed operations that are not part of the
core library.

## Merging

Spread expressions merge objects shallowly, later keys winning:

```ucode
let defaults = { theme: "light", fontSize: 12, notifications: true };
let user = { theme: "dark" };

let merged = { ...defaults, ...user };

print(merged, "\n");
// { "theme": "dark", "fontSize": 12, "notifications": true }
```

For nested objects a recursive version is needed; note the forward
declaration, as ucode does not hoist function declarations:

```ucode
function deepMergeArray;

function deepMerge(target, ...sources) {
    for (let source in sources) {
        if (type(source) != "object")
            continue;

        for (let key in keys(source)) {
            if (type(source[key]) == "object" && type(target[key]) == "object")
                target[key] = deepMerge({ ...target[key] }, source[key]);
            else if (type(source[key]) == "array" && type(target[key]) == "array")
                target[key] = deepMergeArray(target[key], source[key]);
            else
                target[key] = source[key];
        }
    }

    return target;
}

function deepMergeArray(target, source) {
    for (let i = 0; i < length(source); i++) {
        if (type(source[i]) == "object" && type(target[i]) == "object")
            target[i] = deepMerge({ ...target[i] }, source[i]);
        else if (type(source[i]) == "array" && type(target[i]) == "array")
            target[i] = deepMergeArray(target[i], source[i]);
        else
            target[i] = source[i];
    }

    return target;
}

let profile = {
    name: "Alice",
    preferences: { theme: "light", sidebar: { visible: true, width: 250 } }
};

let updates = { preferences: { theme: "dark", sidebar: { width: 300 } } };

deepMerge({}, profile, updates);
/*
{
    "name": "Alice",
    "preferences": {
        "theme": "dark",
        "sidebar": { "visible": true, "width": 300 }
    }
}
*/
```

## Filtering and mapping

```ucode
function filterObject(obj, filterFn) {
    let result = {};

    for (let key in keys(obj))
        if (filterFn(key, obj[key]))
            result[key] = obj[key];

    return result;
}

let mixed = { a: 1, b: "string", c: 3, d: true, e: 4.5 };

filterObject(mixed, (key, value) =>
    type(value) == "int" || type(value) == "double");
// { "a": 1, "c": 3, "e": 4.5 }
```

```ucode
function mapObject(obj, mapFn) {
    let result = {};

    for (let key in keys(obj))
        result[key] = mapFn(key, obj[key]);

    return result;
}

mapObject({ apple: 1.25, banana: 0.75, cherry: 2.50 },
          (fruit, price) => price * 0.8);
// { "apple": 1, "banana": 0.6, "cherry": 2 }
```

## Caches and lookup tables

Objects make natural memoization caches, captured by closure:

```ucode
let fibonacci = (function () {
    let cache = {};

    return function fib(n) {
        if (exists(cache, n))
            return cache[n];

        let result = (n <= 1) ? n : fib(n - 1) + fib(n - 2);
        cache[n] = result;
        return result;
    };
})();

fibonacci(40);   // 102334155
```

Lookup tables replace chains of conditionals, with `??` supplying the
fallback:

```ucode
let statusMessages = { "200": "OK", "404": "Not Found", "500": "Server Error" };

let getStatus = (code) => statusMessages[code] ?? "Unknown";

getStatus(404);   // "Not Found"
getStatus(999);   // "Unknown"
```

## Deep cloning

A deep clone re-uses the array and object halves from
[the arrays page](tutorial-02-arrays.html); the object half, adapted to start
from an object:

```ucode
function deepCloneArray;

function deepClone(obj) {
    if (type(obj) != "object")
        return obj;

    let clone = {};

    for (let key in keys(obj)) {
        if (type(obj[key]) == "object")
            clone[key] = deepClone(obj[key]);
        else if (type(obj[key]) == "array")
            clone[key] = deepCloneArray(obj[key]);
        else
            clone[key] = obj[key];
    }

    return clone;
}
```

## Comparing by value

Equality operators test reference identity; structural comparison is a
recipe, again forward-declared to break the mutual recursion:

```ucode
function arrayEquals;

function objectEquals(obj1, obj2) {
    if (type(obj1) != "object" || type(obj2) != "object")
        return obj1 === obj2;

    let keys1 = keys(obj1);
    let keys2 = keys(obj2);

    if (length(keys1) != length(keys2))
        return false;

    for (let key in keys1) {
        if (!exists(obj2, key))
            return false;

        if (type(obj1[key]) == "object" && type(obj2[key]) == "object") {
            if (!objectEquals(obj1[key], obj2[key]))
                return false;
        } else if (type(obj1[key]) == "array" && type(obj2[key]) == "array") {
            if (!arrayEquals(obj1[key], obj2[key]))
                return false;
        } else if (obj1[key] !== obj2[key]) {
            return false;
        }
    }

    return true;
}

function arrayEquals(arr1, arr2) {
    if (length(arr1) != length(arr2))
        return false;

    for (let i = 0; i < length(arr1); i++) {
        if (type(arr1[i]) == "object" && type(arr2[i]) == "object") {
            if (!objectEquals(arr1[i], arr2[i]))
                return false;
        } else if (type(arr1[i]) == "array" && type(arr2[i]) == "array") {
            if (!arrayEquals(arr1[i], arr2[i]))
                return false;
        } else if (arr1[i] !== arr2[i]) {
            return false;
        }
    }

    return true;
}
```

## Iterating and sorting

The two-variable `for ... in` form destructures key and value:

```ucode
let inventory = { apples: 50, oranges: 25, bananas: 30 };

for (let fruit, quantity in inventory)
    printf("We have %d %s in stock\n", quantity, fruit);
```

Objects keep their keys in insertion order, and `sort()` reorders them in
place; the compare function receives both keys and both values:

```ucode
let stock = { apples: 45, bananas: 25, oranges: 30, grapes: 60 };

sort(stock, (k1, k2, v1, v2) => v2 - v1);

for (let fruit, quantity in stock)
    printf("%s: %d\n", fruit, quantity);
// grapes: 60
// apples: 45
// oranges: 30
// bananas: 25
```
