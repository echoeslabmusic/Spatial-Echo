# Third-Party Notices

Spatial Echo is open-source software released under the MIT License; see the
`LICENSE` file in the source repository
(https://github.com/echoeslabmusic/Spatial-Echo).
Copyright (c) 2024-2026 Echoes Lab.

The Spatial Echo name and the Echoes Lab logos and brand assets
(`plugin/Source/Resources/Brand/*.svg`) are trademarks of Echoes Lab and are
not covered by the MIT License. The logo files themselves remain copyright
Echoes Lab, all rights reserved. If you distribute a modified build, give it
its own name, bundle identifier (the official build uses
`com.echoeslab.spatialecho`), Audio Unit manufacturer and subtype codes (the
official build uses `EchL` and `SpEc`), and installer AppId, so that your build
does not collide with the official one inside hosts.

This product incorporates the following third-party software, data, and
assets. Their respective licenses and copyright notices are reproduced below
as required by each license.

This software is based in part on the work of the Independent JPEG Group.

VST is a registered trademark of Steinberg Media Technologies GmbH. Audio
Units is a trademark of Apple Inc.

---

## CloudSeed: Algorithmic Reverb

**License:** MIT

The Cloud reverb implementation is a CloudSeed-derived port. The vendored
`CloudSeedCore` source carries the Ghost Note Engineering copyright notice;
the original CloudSeed project is credited to Valdemar Orn.

**Copyright (c) Valdemar Orn**

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

**Copyright (c) 2024 Ghost Note Engineering Ltd**

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

---

## VST 3 SDK

**License:** MIT

VST is a registered trademark of Steinberg Media Technologies GmbH.

**Copyright (c) 2025 Steinberg Media Technologies GmbH**

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

---

## libmysofa: SOFA File Reader

**License:** BSD 3-Clause

**Copyright (c) 2016 Symonics GmbH, Christian Hoene.** All rights reserved.

The copy vendored under `plugin/Source/DSP/dependencies/sofa` uses the
flattened packaging of libmysofa from the Spatial Audio Framework (SAF) by
Leo McCormack, https://github.com/leomccormack/Spatial_Audio_Framework. The
files keep the libmysofa and kdtree notices reproduced here.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. The name of the author may not be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR "AS IS" AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

### kdtree (bundled within libmysofa)

**Copyright (c) 2007-2011 John Tsiombikas.** BSD 3-Clause. Same terms as above.

---

## chowdsp_wdf: Wave Digital Filters

**License:** BSD 3-Clause, with an MIT-licensed Wright Omega implementation

**Copyright (c) 2022 Chowdhury-DSP (Jatin Chowdhury).** All rights reserved.

The copy vendored under `plugin/Source/DSP/dependencies/chowdsp_wdf` does not
include a LICENSE file. The BSD 3-Clause terms below are reproduced from the
upstream repository, https://github.com/Chowdhury-DSP/chowdsp_wdf. The Wright
Omega header inside it carries Stefano D'Angelo's separate MIT notice, which
follows.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

### Wright Omega implementation used by chowdsp_wdf

The bundled `plugin/Source/DSP/dependencies/chowdsp_wdf/math/omega.h` header
contains Stefano D'Angelo's MIT-licensed Wright Omega approximation, modified
by Jatin Chowdhury.

**Copyright (C) 2019 Stefano D'Angelo**

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

---

## zlib: Compression Library

**License:** zlib License (bundled version 1.3.2)

**Copyright (c) 1995-2026 Jean-loup Gailly and Mark Adler**

This software is provided 'as-is', without any express or implied warranty.
In no event will the authors be held liable for any damages arising from the
use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not claim
   that you wrote the original software. If you use this software in a
   product, an acknowledgment in the product documentation would be
   appreciated but is not required.

2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

3. This notice may not be removed or altered from any source distribution.

JUCE compiles in its own copy of zlib (version 1.3.1, Copyright (C) 1995-2024
Jean-loup Gailly and Mark Adler) under the same license.

---

## Ooura FFT

**License:** Author's permissive terms

**Author:** Takuya Ooura, Kyoto University
https://www.kurims.kyoto-u.ac.jp/~ooura/fft.html

Takuya Ooura's FFT package may be used, copied, modified, and distributed for
any purpose without fee. The package is provided without warranty. See the
author's page above for the original statement.

---

## PCG32: Random Number Generator

**License:** Apache License, Version 2.0 (full text in `APACHE-2.0.txt`,
installed next to this file)

**Copyright 2014 Melissa O'Neill <oneill@pcg-random.org>**
https://www.pcg-random.org

`plugin/Source/DSP/PCG32.h` adapts the seeding and output functions of the PCG
minimal C implementation (`pcg_basic.c`). Echoes Lab changes: wrapped in a C++
class, added float output and random-device seeding.

Licensed under the Apache License, Version 2.0 (the "License"); you may not use
this file except in compliance with the License. You may obtain a copy of the
License at http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software distributed
under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied. See the License for the
specific language governing permissions and limitations under the License.

---

## HUTUBS HRTF Dataset

**License:** Creative Commons Attribution 4.0 International (CC BY 4.0)
https://creativecommons.org/licenses/by/4.0/

This product bundles the HUTUBS measured HRIR dataset
(`pp1_HRIRs_measured.sofa`). Attribution is required by the license. The
bundled file has SHA-256
`f626d335c590db687608fff5992972725230d594067b2e6906893bddc85e5173`.

**Citations:**

- Fabian Brinkmann, Manoj Dinakaran, Robert Pelzer, Peter Grosche,
  Daniel Voss, and Stefan Weinzierl (2019). "A Cross-validated database of
  measured and simulated HRTFs including 3D head meshes and anthropometric
  features." *Journal of the Audio Engineering Society.*

- Fabian Brinkmann, Manoj Dinakaran, Robert Pelzer, Jan Joschka Wohlgemuth,
  Fabian Seipl, and Stefan Weinzierl. "The HUTUBS HRTF database."
  DOI: 10.14279/depositonce-8487

**Dataset source:** https://sofacoustics.org/data/database/hutubs/

---

## PxPlus IBM VGA 9x16 Font

**Author:** VileR

**License:** Creative Commons Attribution-ShareAlike 4.0 International
(CC BY-SA 4.0)
https://creativecommons.org/licenses/by-sa/4.0/

**Source:** int10h.org Oldschool PC Font Pack
https://int10h.org/oldschool-pc-fonts/

The font file is bundled unmodified. The ShareAlike obligation applies only to
the font file itself and its derivatives, not to Spatial Echo as a whole.

---

## Wireframe Head Contours (SpatialHeadView)

**License:** Public domain / CC0, as distributed via
https://github.com/alecjacobson/common-3d-test-models
("Max Planck bust", MPI Informatik).

The 3D head drawn in the spatial display is generated from
`plugin/Source/UI/Components/HeadWireframeData.h`: 22 contour polylines
(515 points) extracted by contour-slicing the "Max Planck bust" graphics
test model. The shipped data is a heavily simplified original derivative
(sliced contour polylines), not the original mesh; no mesh data is
distributed.

---

## JUCE Framework

**License:** Dual-licensed under the GNU Affero General Public License v3
and the JUCE commercial licence (https://juce.com/legal/juce-8-licence/)

**Copyright (c) Raw Material Software Limited**
https://juce.com

JUCE is not included in this repository. The build uses a separate checkout
of the official JUCE repository at the revision pinned in
`cmake/SpatialEchoJucePin.cmake`. Official Echoes Lab binaries are built under
the JUCE commercial (Starter) licence. Anyone who builds or redistributes their
own binaries must comply with the JUCE licence that applies to them. If you
build Spatial Echo yourself without a JUCE commercial licence, your binary is
an AGPLv3 work and the AGPLv3 applies to it.

---

## Libraries Compiled In From JUCE

The plug-in and standalone binaries include the following code that ships
inside the JUCE modules. JUCE lists these dependencies in its own
`LICENSE.md`.

### HarfBuzz: Text Shaping

**License:** "Old MIT"

HarfBuzz is licensed under the so-called "Old MIT" license.

- Copyright © 2010-2022  Google, Inc.
- Copyright © 2015-2020  Ebrahim Byagowi
- Copyright © 2019,2020  Facebook, Inc.
- Copyright © 2012,2015  Mozilla Foundation
- Copyright © 2011  Codethink Limited
- Copyright © 2008,2010  Nokia Corporation and/or its subsidiary(-ies)
- Copyright © 2009  Keith Stribley
- Copyright © 2011  Martin Hosken and SIL International
- Copyright © 2007  Chris Wilson
- Copyright © 2005,2006,2020,2021,2022,2023  Behdad Esfahbod
- Copyright © 2004,2007,2008,2009,2010,2013,2021,2022,2023  Red Hat, Inc.
- Copyright © 1998-2005  David Turner and Werner Lemberg
- Copyright © 2016  Igalia S.L.
- Copyright © 2022  Matthias Clasen
- Copyright © 2018,2021  Khaled Hosny
- Copyright © 2018,2019,2020  Adobe, Inc
- Copyright © 2013-2015  Alexei Podtelezhnikov

For full copyright notices consult the individual files in the package.

Permission is hereby granted, without written agreement and without
license or royalty fees, to use, copy, modify, and distribute this
software and its documentation for any purpose, provided that the
above copyright notice and the following two paragraphs appear in
all copies of this software.

IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE TO ANY PARTY FOR
DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES
ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN
IF THE COPYRIGHT HOLDER HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH
DAMAGE.

THE COPYRIGHT HOLDER SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,
BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
ON AN "AS IS" BASIS, AND THE COPYRIGHT HOLDER HAS NO OBLIGATION TO
PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.

### SheenBidi: Unicode Bidirectional Algorithm

**License:** Apache License, Version 2.0 (full text in `APACHE-2.0.txt`,
installed next to this file)

**Copyright (C) 2014-2025 Muhammad Tayyab Akram**
https://github.com/Tehreer/SheenBidi

### Audio Unit SDK (Audio Unit builds only)

**License:** Apache License, Version 2.0 (full text in `APACHE-2.0.txt`,
installed next to this file)

**Copyright © 2000-2021 Apple Inc.** All rights reserved.
https://github.com/apple/AudioUnitSDK

### The Independent JPEG Group's JPEG Software

This software is based in part on the work of the Independent JPEG Group.

**Copyright (C) 1991-2024, Thomas G. Lane, Guido Vollbeding.** All Rights
Reserved except as specified below.

The authors make NO WARRANTY or representation, either express or implied,
with respect to this software, its quality, accuracy, merchantability, or
fitness for a particular purpose.  This software is provided "AS IS", and you,
its user, assume the entire risk as to its quality and accuracy.

Permission is hereby granted to use, copy, modify, and distribute this
software (or portions thereof) for any purpose, without fee, subject to these
conditions:
(1) If any part of the source code for this software is distributed, then this
README file must be included, with this copyright and no-warranty notice
unaltered; and any additions, deletions, or changes to the original files
must be clearly indicated in accompanying documentation.
(2) If only executable code is distributed, then the accompanying
documentation must state that "this software is based in part on the work of
the Independent JPEG Group".
(3) Permission for use of this software is granted only if the user accepts
full responsibility for any undesirable consequences; the authors accept
NO LIABILITY for damages of any kind.

### libpng: PNG Reference Library

**License:** PNG Reference Library License version 2

 * Copyright (c) 1995-2019 The PNG Reference Library Authors.
 * Copyright (c) 2018-2019 Cosmin Truta.
 * Copyright (c) 2000-2002, 2004, 2006-2018 Glenn Randers-Pehrson.
 * Copyright (c) 1996-1997 Andreas Dilger.
 * Copyright (c) 1995-1996 Guy Eric Schalnat, Group 42, Inc.

The software is supplied "as is", without warranty of any kind,
express or implied, including, without limitation, the warranties
of merchantability, fitness for a particular purpose, title, and
non-infringement.  In no event shall the Copyright owners, or
anyone distributing the software, be liable for any damages or
other liability, whether in contract, tort or otherwise, arising
from, out of, or in connection with the software, or the use or
other dealings in the software, even if advised of the possibility
of such damage.

Permission is hereby granted to use, copy, modify, and distribute
this software, or portions hereof, for any purpose, without fee,
subject to the following restrictions:

 1. The origin of this software must not be misrepresented; you
    must not claim that you wrote the original software.  If you
    use this software in a product, an acknowledgment in the product
    documentation would be appreciated, but is not required.

 2. Altered source versions must be plainly marked as such, and must
    not be misrepresented as being the original software.

 3. This Copyright notice may not be removed or altered from any
    source or altered source distribution.

### GLEW, Mesa, and Khronos OpenGL Headers

JUCE's OpenGL header (`juce_opengl/opengl/juce_gl.h`) contains a portion
taken from the OpenGL Extension Wrangler Library, which carries the following
notices.

**The OpenGL Extension Wrangler Library**

- Copyright (C) 2002-2007, Milan Ikits <milan ikits[]ieee org>
- Copyright (C) 2002-2007, Marcelo E. Magallon <mmagallo[]debian org>
- Copyright (C) 2002, Lev Povalahev

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice,
  this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.
* The name of the author may be used to endorse or promote products
  derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
THE POSSIBILITY OF SUCH DAMAGE.

**Mesa 3-D graphics library**
Version:  7.0
Copyright (C) 1999-2007  Brian Paul   All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
BRIAN PAUL BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

**Copyright (c) 2007 The Khronos Group Inc.**

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and/or associated documentation files (the
"Materials"), to deal in the Materials without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Materials, and to
permit persons to whom the Materials are furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Materials.

THE MATERIALS ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
MATERIALS OR THE USE OR OTHER DEALINGS IN THE MATERIALS.
