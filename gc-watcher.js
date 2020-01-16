'use strict';

/* eslint-disable no-console */
const watcher = require('bindings')('gc-watcher');

const throwError = process.argv.indexOf('error') > 1;
const callbacks = throwError || process.argv.indexOf('callbacks') > 1;

let status;
if (callbacks) {
  status = watcher.enable(before, after);
} else {
  status = watcher.enable();
  console.log(status);
}

let cumET = 0;

function before () {console.log('>>', ...arguments)}
function after (type, flags, deltaTime, gcCount, error) {
  console.log('<<', ...arguments)
  console.log('type', type, 'error', error, 'et', deltaTime, 'cet', cumET);
  try {
    if (throwError) throw new Error('oops');
  } catch (e) {
    console.log('caught it');
  }
  cumET += deltaTime;
}


const a = [];
for (let i = 0; i < 1000000; i++) {
  a[i] = i * i;
}
cumET = 0;
console.log(watcher.getCumulative());
a.length = 0;
for (let i = 0; i < 1000000; i++) {
  a[i] = i * i;
}
cumET = 0;
console.log(watcher.getCumulative());
a.length = 0;
for (let i = 0; i < 1000000; i++) {
  a[i] = i * i;
}
cumET = 0;
console.log(watcher.getCumulative());
