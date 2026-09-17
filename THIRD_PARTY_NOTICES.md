# Third-party references and notices

## Evil-M5Project — 7h30th3r0n3

The Network Tools and Flipper-like BLE service hints reference the feature designs
in [Evil-Cardputer-v1-5-5.ino](https://github.com/7h30th3r0n3/Evil-M5Project/blob/main/Evil-Cardputer-v1-5-5.ino),
reviewed on 2026-09-11. Relevant sections include `scanHosts`, `scanPorts`, the
CCTV toolkit, `detectPrinter`, `sipScan`, `listUPnPMappings`, and Wall of Flippers.
Thanks to **7h30th3r0n3** for making that work available.

AWOKxDAG's network engine, parsers, and UI integration are newly written for this
firmware. Upstream scanner function bodies were not copied. Advertised BLE service
UUIDs serve as protocol identification hints. Evil-M5's `scanPorts` section credits
[pr3y/Bruce](https://github.com/pr3y/bruce); that section was not transplanted.
Do not assume the sketch's top-level notice supersedes notices on third-party
code or dependencies.

The reviewed Evil-Cardputer sketch includes this MIT notice:

```text
Copyright (c) 2026 7h30th3r0n3

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
