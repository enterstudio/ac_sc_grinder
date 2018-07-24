#!/usr/bin/env node

/*
  Build `./src/fix16_math/fix16_sinusize_table.h
*/

const TABLE_SIZE = 512;

const path = require('path');
const write = require('fs').writeFileSync;

function to_fix16(x) {
  //return x;
  return Math.floor((x * 65536) + 0.5);
}

let res = [];

for (let i=0; i < TABLE_SIZE; i++) {
  res[i] = to_fix16((Math.asin(-1.0 + i * 2 / (TABLE_SIZE - 1)) * 2 / Math.PI + 1) / 2);

  // hacky clamp
  if (res[i] > 65535) res[i] = 65535;

  //res[i] = res[i] >> 1;
}

const out = `#ifndef __FIX16_SINUSIZE_TABLE__
#define __FIX16_SINUSIZE_TABLE__

// This is autogenerated file, with EEPROM map & defaults.
// Use \`npm run sinusize\` to regenerate.

#define SINUSIZE_TABLE_SIZE ${TABLE_SIZE}
#define SINUSIZE_TABLE_SIZE_BITS ${Math.round(Math.log2(TABLE_SIZE))}


static const uint16_t sinusize_table[SINUSIZE_TABLE_SIZE] = {
${res.join(',\n')}
};


#endif
`;


write(path.resolve(__dirname, '../src/fix16_math/fix16_sinusize_table.h'), out);