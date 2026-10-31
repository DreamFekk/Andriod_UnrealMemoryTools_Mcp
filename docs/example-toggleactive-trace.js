// Generated for this exact dump. Verify the same game build before attaching.
// x0..x3 are raw ARM64 argument registers, not recovered typed arguments.
'use strict';
const moduleName = "libUE4.so";
const rva = ptr("0x1288eee0");
const image = Process.getModuleByName(moduleName);
const address = image.base.add(rva);
const range = Process.findRangeByAddress(address);
if (!range || !range.protection.includes('x')) throw new Error('RVA is not executable');
let remaining = 30;
Interceptor.attach(address, {
  onEnter(args) {
    this.sample = remaining-- > 0;
    if (this.sample) console.log(JSON.stringify({name:"ActorComponent$$execToggleActive", address:address.toString(), thread:this.threadId,
      x0:args[0].toString(), x1:args[1].toString(), x2:args[2].toString(), x3:args[3].toString()}));
  },
  onLeave(value) { if (this.sample) console.log('return(raw): ' + value); }
});
