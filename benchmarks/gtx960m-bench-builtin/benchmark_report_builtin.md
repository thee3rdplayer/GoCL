# Vulkan Benchmark Comparison (Sascha Willems Built-in Benchmark Mode)
**Date:** Wed  5 Aug 05:26:50 CAT 2026

**Hardware Model:** Lenovo IdeaPad Y700-15ISK (80NV), **GPU:** GeForce GTX 960M 4GB GDDR5, **Driver ver.:** 580.173.02, **OS:** Arch Linux, **Kernel ver.:** 7.1.5-1-cachyos-bore, **Scheduler:** scx_lavd 1.1.2

Each example was run using the sample framework's own benchmark harness
(`-b -bw 5 -br 120 -bf <file> -bt`)

**Note:** the built-in harness only reports frame timings/FPS — it does not expose GPU load,
CPU load, or VRAM usage the way MangoHud did, so those columns are gone, not just empty.

Three configurations:
- **Native** – no proxy
- **Proxy Default** – GoCL proxy with stock settings
- **Proxy Tuned** – GoCL proxy with max\_frames\_in\_flight=3, immediate present mode

| Example | Config | Avg FPS | Δ | 1% Low FPS | Δ | 0.1% Low FPS | Δ | Frame Time (ms) | Δ | Frames |
|---------|--------|---------|---|------------|---|--------------|---|------------------|---|--------|
| raytracingreflections | Native | NA | — | NA | — | NA | — | NA | — | NA |
| raytracingreflections | Proxy Default | NA | — | NA | — | NA | — | NA | — | NA |
| raytracingreflections | Proxy Tuned | NA | — | NA | — | NA | — | NA | — | NA |
| shadowmappingomni | Native | 609.7237 | — | 451.0322 | — | 170.6801 | — | 1.6401 | — | 73167 |
| shadowmappingomni | Proxy Default | 607.1412 | <span style="color:red">-0.4%</span> | 309.1315 | <span style="color:red">-31.5%</span> | 62.6305 | <span style="color:red">-63.3%</span> | 1.6471 | <span style="color:red">+0.4%</span> | 72857 |
| shadowmappingomni | Proxy Tuned | 597.3740 | <span style="color:red">-2.0%</span> | 146.4939 | <span style="color:red">-67.5%</span> | 31.9858 | <span style="color:red">-81.3%</span> | 1.6740 | <span style="color:red">+2.1%</span> | 71685 |
| displacement | Native | 890.9944 | — | 293.7124 | — | 182.7980 | — | 1.1223 | — | 106920 |
| displacement | Proxy Default | 888.4076 | <span style="color:red">-0.3%</span> | 280.7346 | <span style="color:red">-4.4%</span> | 153.0304 | <span style="color:red">-16.3%</span> | 1.1256 | <span style="color:red">+0.3%</span> | 106609 |
| displacement | Proxy Tuned | 887.5694 | <span style="color:red">-0.4%</span> | 280.9043 | <span style="color:red">-4.4%</span> | 156.6052 | <span style="color:red">-14.3%</span> | 1.1267 | <span style="color:red">+0.4%</span> | 106509 |
| computecloth | Native | 971.4935 | — | 290.9391 | — | 187.4551 | — | 1.0293 | — | 116580 |
| computecloth | Proxy Default | 946.4478 | <span style="color:red">-2.6%</span> | 293.8085 | <span style="color:green">+1.0%</span> | 190.9364 | <span style="color:green">+1.9%</span> | 1.0566 | <span style="color:red">+2.7%</span> | 113574 |
| computecloth | Proxy Tuned | 970.7644 | <span style="color:red">-0.1%</span> | 289.8477 | <span style="color:red">-0.4%</span> | 184.5205 | <span style="color:red">-1.6%</span> | 1.0301 | <span style="color:red">+0.1%</span> | 116492 |
| texture3d | Native | 852.2255 | — | 237.2177 | — | 141.7666 | — | 1.1734 | — | 102268 |
| texture3d | Proxy Default | 854.3412 | <span style="color:green">+0.2%</span> | 239.9703 | <span style="color:green">+1.2%</span> | 145.3543 | <span style="color:green">+2.5%</span> | 1.1705 | <span style="color:green">-0.2%</span> | 102521 |
| texture3d | Proxy Tuned | 849.6376 | <span style="color:red">-0.3%</span> | 237.5590 | <span style="color:green">+0.1%</span> | 142.7701 | <span style="color:green">+0.7%</span> | 1.1770 | <span style="color:red">+0.3%</span> | 101957 |
| pushconstants | Native | 990.3960 | — | 300.1675 | — | 192.6753 | — | 1.0097 | — | 118848 |
| pushconstants | Proxy Default | 989.5074 | <span style="color:red">-0.1%</span> | 298.3441 | <span style="color:red">-0.6%</span> | 186.0229 | <span style="color:red">-3.5%</span> | 1.0106 | <span style="color:red">+0.1%</span> | 118741 |
| pushconstants | Proxy Tuned | 990.4480 | <span style="color:grey">+0.0%</span> | 298.7212 | <span style="color:red">-0.5%</span> | 191.2143 | <span style="color:red">-0.8%</span> | 1.0096 | <span style="color:grey">-0.0%</span> | 118854 |
| dynamicrenderinglocalread | Native | 41.9409 | — | 40.8839 | — | 40.0606 | — | 23.8431 | — | 5033 |
| dynamicrenderinglocalread | Proxy Default | 41.7855 | <span style="color:red">-0.4%</span> | 40.8274 | <span style="color:red">-0.1%</span> | 40.4401 | <span style="color:green">+0.9%</span> | 23.9318 | <span style="color:red">+0.4%</span> | 5015 |
| dynamicrenderinglocalread | Proxy Tuned | 41.8712 | <span style="color:red">-0.2%</span> | 40.8945 | <span style="color:grey">+0.0%</span> | 40.6437 | <span style="color:green">+1.5%</span> | 23.8827 | <span style="color:red">+0.2%</span> | 5025 |
| viewportarray | Native | 980.9961 | — | 250.9721 | — | 160.2871 | — | 1.0194 | — | 117720 |
| viewportarray | Proxy Default | 974.9533 | <span style="color:red">-0.6%</span> | 259.7959 | <span style="color:green">+3.5%</span> | 154.9597 | <span style="color:red">-3.3%</span> | 1.0257 | <span style="color:red">+0.6%</span> | 116995 |
| viewportarray | Proxy Tuned | 970.6518 | <span style="color:red">-1.1%</span> | 262.5703 | <span style="color:green">+4.6%</span> | 159.0139 | <span style="color:red">-0.8%</span> | 1.0302 | <span style="color:red">+1.1%</span> | 116479 |
| deferredshadows | Native | 108.7560 | — | 99.8414 | — | 95.9691 | — | 9.1949 | — | 13051 |
| deferredshadows | Proxy Default | 108.8489 | <span style="color:green">+0.1%</span> | 99.7877 | <span style="color:red">-0.1%</span> | 94.5131 | <span style="color:red">-1.5%</span> | 9.1870 | <span style="color:green">-0.1%</span> | 13062 |
| deferredshadows | Proxy Tuned | 108.8497 | <span style="color:green">+0.1%</span> | 100.2587 | <span style="color:green">+0.4%</span> | 97.7487 | <span style="color:green">+1.9%</span> | 9.1870 | <span style="color:green">-0.1%</span> | 13062 |
| computecullandlod | Native | 49.3478 | — | 48.4472 | — | 48.0958 | — | 20.2643 | — | 5922 |
| computecullandlod | Proxy Default | 49.3478 | <span style="color:grey">+0.0%</span> | 48.4183 | <span style="color:red">-0.1%</span> | 47.7314 | <span style="color:red">-0.8%</span> | 20.2643 | <span style="color:grey">+0.0%</span> | 5922 |
| computecullandlod | Proxy Tuned | 49.3597 | <span style="color:grey">+0.0%</span> | 48.4700 | <span style="color:grey">+0.0%</span> | 47.9810 | <span style="color:red">-0.2%</span> | 20.2595 | <span style="color:grey">-0.0%</span> | 5924 |
| meshshader | Native | NA | — | NA | — | NA | — | NA | — | NA |
| meshshader | Proxy Default | NA | — | NA | — | NA | — | NA | — | NA |
| meshshader | Proxy Tuned | NA | — | NA | — | NA | — | NA | — | NA |
| texturecubemaparray | Native | 818.2606 | — | 151.8071 | — | 107.2606 | — | 1.2221 | — | 98192 |
| texturecubemaparray | Proxy Default | 785.6678 | <span style="color:red">-4.0%</span> | 124.6378 | <span style="color:red">-17.9%</span> | 49.5122 | <span style="color:red">-53.8%</span> | 1.2728 | <span style="color:red">+4.1%</span> | 94281 |
| texturecubemaparray | Proxy Tuned | 804.7223 | <span style="color:red">-1.7%</span> | 147.4295 | <span style="color:red">-2.9%</span> | 101.3283 | <span style="color:red">-5.5%</span> | 1.2427 | <span style="color:red">+1.7%</span> | 96567 |
| multisampling | Native | 205.6908 | — | 189.4595 | — | 176.1289 | — | 4.8617 | — | 24683 |
| multisampling | Proxy Default | 205.6896 | <span style="color:grey">-0.0%</span> | 189.0406 | <span style="color:red">-0.2%</span> | 175.1948 | <span style="color:red">-0.5%</span> | 4.8617 | <span style="color:grey">+0.0%</span> | 24683 |
| multisampling | Proxy Tuned | 205.6915 | <span style="color:grey">+0.0%</span> | 189.3595 | <span style="color:red">-0.1%</span> | 171.8182 | <span style="color:red">-2.4%</span> | 4.8616 | <span style="color:grey">-0.0%</span> | 24683 |
| pbrtexture | Native | 872.8281 | — | 313.6060 | — | 161.0077 | — | 1.1457 | — | 104740 |
| pbrtexture | Proxy Default | 870.7274 | <span style="color:red">-0.2%</span> | 306.7424 | <span style="color:red">-2.2%</span> | 163.5398 | <span style="color:green">+1.6%</span> | 1.1485 | <span style="color:red">+0.2%</span> | 104488 |
| pbrtexture | Proxy Tuned | 868.8030 | <span style="color:red">-0.5%</span> | 306.5599 | <span style="color:red">-2.2%</span> | 159.5349 | <span style="color:red">-0.9%</span> | 1.1510 | <span style="color:red">+0.5%</span> | 104257 |
| raytracingbasic | Native | NA | — | NA | — | NA | — | NA | — | NA |
| raytracingbasic | Proxy Default | NA | — | NA | — | NA | — | NA | — | NA |
| raytracingbasic | Proxy Tuned | NA | — | NA | — | NA | — | NA | — | NA |
| inlineuniformblocks | Native | 982.1525 | — | 290.4690 | — | 180.5524 | — | 1.0182 | — | 117861 |
| inlineuniformblocks | Proxy Default | 982.5946 | <span style="color:grey">+0.0%</span> | 290.8570 | <span style="color:green">+0.1%</span> | 183.0778 | <span style="color:green">+1.4%</span> | 1.0177 | <span style="color:grey">-0.0%</span> | 117912 |
| inlineuniformblocks | Proxy Tuned | 983.2619 | <span style="color:green">+0.1%</span> | 291.6172 | <span style="color:green">+0.4%</span> | 182.0066 | <span style="color:green">+0.8%</span> | 1.0170 | <span style="color:green">-0.1%</span> | 117992 |
| gltfloading | Native | 974.2446 | — | 295.6734 | — | 182.7007 | — | 1.0264 | — | 116910 |
| gltfloading | Proxy Default | 974.8077 | <span style="color:green">+0.1%</span> | 293.4494 | <span style="color:red">-0.8%</span> | 171.5621 | <span style="color:red">-6.1%</span> | 1.0258 | <span style="color:green">-0.1%</span> | 116977 |
| gltfloading | Proxy Tuned | 975.9383 | <span style="color:green">+0.2%</span> | 295.9778 | <span style="color:green">+0.1%</span> | 181.1708 | <span style="color:red">-0.8%</span> | 1.0247 | <span style="color:green">-0.2%</span> | 117113 |
| negativeviewportheight | Native | 991.3580 | — | 298.6889 | — | 189.3387 | — | 1.0087 | — | 118963 |
| negativeviewportheight | Proxy Default | 986.2737 | <span style="color:red">-0.5%</span> | 298.0048 | <span style="color:red">-0.2%</span> | 191.6757 | <span style="color:green">+1.2%</span> | 1.0139 | <span style="color:red">+0.5%</span> | 118355 |
| negativeviewportheight | Proxy Tuned | 988.8238 | <span style="color:red">-0.3%</span> | 297.8384 | <span style="color:red">-0.3%</span> | 185.0611 | <span style="color:red">-2.3%</span> | 1.0113 | <span style="color:red">+0.3%</span> | 118659 |
| debugutils | Native | 988.3679 | — | 290.3444 | — | 186.3254 | — | 1.0118 | — | 118605 |
| debugutils | Proxy Default | 987.2888 | <span style="color:red">-0.1%</span> | 290.1516 | <span style="color:red">-0.1%</span> | 189.0660 | <span style="color:green">+1.5%</span> | 1.0129 | <span style="color:red">+0.1%</span> | 118476 |
| debugutils | Proxy Tuned | 987.4450 | <span style="color:red">-0.1%</span> | 291.2544 | <span style="color:green">+0.3%</span> | 187.4870 | <span style="color:green">+0.6%</span> | 1.0127 | <span style="color:red">+0.1%</span> | 118496 |
| texturearray | Native | 874.1795 | — | 244.4870 | — | 138.8678 | — | 1.1439 | — | 104902 |
| texturearray | Proxy Default | 850.8140 | <span style="color:red">-2.7%</span> | 198.9959 | <span style="color:red">-18.6%</span> | 124.5377 | <span style="color:red">-10.3%</span> | 1.1753 | <span style="color:red">+2.7%</span> | 102098 |
| texturearray | Proxy Tuned | 853.1070 | <span style="color:red">-2.4%</span> | 186.5951 | <span style="color:red">-23.7%</span> | 62.7749 | <span style="color:red">-54.8%</span> | 1.1722 | <span style="color:red">+2.5%</span> | 102376 |
| raytracingshadows | Native | NA | — | NA | — | NA | — | NA | — | NA |
| raytracingshadows | Proxy Default | NA | — | NA | — | NA | — | NA | — | NA |
| raytracingshadows | Proxy Tuned | NA | — | NA | — | NA | — | NA | — | NA |
| raytracinggltf | Native | NA | — | NA | — | NA | — | NA | — | NA |
| raytracinggltf | Proxy Default | NA | — | NA | — | NA | — | NA | — | NA |
| raytracinggltf | Proxy Tuned | NA | — | NA | — | NA | — | NA | — | NA |
| dynamicrendering | Native | 970.7445 | — | 291.8423 | — | 186.9039 | — | 1.0301 | — | 116490 |
| dynamicrendering | Proxy Default | 970.1291 | <span style="color:red">-0.1%</span> | 292.6866 | <span style="color:green">+0.3%</span> | 188.8414 | <span style="color:green">+1.0%</span> | 1.0308 | <span style="color:red">+0.1%</span> | 116417 |
| dynamicrendering | Proxy Tuned | 971.7060 | <span style="color:green">+0.1%</span> | 293.1045 | <span style="color:green">+0.4%</span> | 188.2907 | <span style="color:green">+0.7%</span> | 1.0291 | <span style="color:green">-0.1%</span> | 116606 |
| distancefieldfonts | Native | 969.2442 | — | 289.0757 | — | 182.1262 | — | 1.0317 | — | 116310 |
| distancefieldfonts | Proxy Default | 969.1607 | <span style="color:grey">-0.0%</span> | 289.1862 | <span style="color:grey">+0.0%</span> | 181.6513 | <span style="color:red">-0.3%</span> | 1.0318 | <span style="color:grey">+0.0%</span> | 116300 |
| distancefieldfonts | Proxy Tuned | 970.1667 | <span style="color:green">+0.1%</span> | 298.6926 | <span style="color:green">+3.3%</span> | 214.4602 | <span style="color:green">+17.8%</span> | 1.0308 | <span style="color:green">-0.1%</span> | 116421 |
| texturesparseresidency | Native | NA | — | NA | — | NA | — | NA | — | NA |
| texturesparseresidency | Proxy Default | NA | — | NA | — | NA | — | NA | — | NA |
| texturesparseresidency | Proxy Tuned | NA | — | NA | — | NA | — | NA | — | NA |
| instancing | Native | 381.4090 | — | 294.3848 | — | 134.2868 | — | 2.6219 | — | 45770 |
| instancing | Proxy Default | 379.8108 | <span style="color:red">-0.4%</span> | 310.8331 | <span style="color:green">+5.6%</span> | 183.6298 | <span style="color:green">+36.7%</span> | 2.6329 | <span style="color:red">+0.4%</span> | 45578 |
| instancing | Proxy Tuned | 381.0267 | <span style="color:red">-0.1%</span> | 303.3905 | <span style="color:green">+3.1%</span> | 158.3021 | <span style="color:green">+17.9%</span> | 2.6245 | <span style="color:red">+0.1%</span> | 45724 |
| offscreen | Native | 726.2639 | — | 443.5534 | — | 196.1795 | — | 1.3769 | — | 87152 |
| offscreen | Proxy Default | 726.8476 | <span style="color:green">+0.1%</span> | 439.8188 | <span style="color:red">-0.8%</span> | 192.0203 | <span style="color:red">-2.1%</span> | 1.3758 | <span style="color:green">-0.1%</span> | 87222 |
| offscreen | Proxy Tuned | 725.2792 | <span style="color:red">-0.1%</span> | 444.7931 | <span style="color:green">+0.3%</span> | 200.2351 | <span style="color:green">+2.1%</span> | 1.3788 | <span style="color:red">+0.1%</span> | 87034 |
| fragmentshaderbarycentrics | Native | NA | — | NA | — | NA | — | NA | — | NA |
| fragmentshaderbarycentrics | Proxy Default | NA | — | NA | — | NA | — | NA | — | NA |
| fragmentshaderbarycentrics | Proxy Tuned | NA | — | NA | — | NA | — | NA | — | NA |
| multiview | Native | 466.4204 | — | 359.0414 | — | 173.6303 | — | 2.1440 | — | 55971 |
| multiview | Proxy Default | NA | — | NA | — | NA | — | NA | — | NA |
| multiview | Proxy Tuned | NA | — | NA | — | NA | — | NA | — | NA |
| texture | Native | 981.2662 | — | 296.7094 | — | 198.5736 | — | 1.0191 | — | 117752 |
| texture | Proxy Default | 981.4694 | <span style="color:grey">+0.0%</span> | 296.2962 | <span style="color:red">-0.1%</span> | 192.5738 | <span style="color:red">-3.0%</span> | 1.0189 | <span style="color:grey">-0.0%</span> | 117777 |
| texture | Proxy Tuned | 975.3069 | <span style="color:red">-0.6%</span> | 248.1018 | <span style="color:red">-16.4%</span> | 85.7539 | <span style="color:red">-56.8%</span> | 1.0253 | <span style="color:red">+0.6%</span> | 117075 |
| timelinesemaphore | Native | 146.2055 | — | 100.4397 | — | 92.9631 | — | 6.8397 | — | 17546 |
| timelinesemaphore | Proxy Default | 145.3200 | <span style="color:red">-0.6%</span> | 101.0886 | <span style="color:green">+0.6%</span> | 94.4641 | <span style="color:green">+1.6%</span> | 6.8814 | <span style="color:red">+0.6%</span> | 17439 |
| timelinesemaphore | Proxy Tuned | 145.1997 | <span style="color:red">-0.7%</span> | 101.0366 | <span style="color:green">+0.6%</span> | 95.9887 | <span style="color:green">+3.3%</span> | 6.8871 | <span style="color:red">+0.7%</span> | 17425 |
| screenshot | Native | 1035.2712 | — | 248.4944 | — | 154.8746 | — | 0.9659 | — | 124233 |
| screenshot | Proxy Default | 1049.2452 | <span style="color:green">+1.3%</span> | 243.2445 | <span style="color:red">-2.1%</span> | 160.4597 | <span style="color:green">+3.6%</span> | 0.9531 | <span style="color:green">-1.3%</span> | 125910 |
| screenshot | Proxy Tuned | 1050.8774 | <span style="color:green">+1.5%</span> | 241.7513 | <span style="color:red">-2.7%</span> | 153.7888 | <span style="color:red">-0.7%</span> | 0.9516 | <span style="color:green">-1.5%</span> | 126106 |
| vertexattributes | Native | 381.6194 | — | 285.2030 | — | 118.9506 | — | 2.6204 | — | 45795 |
| vertexattributes | Proxy Default | 379.2040 | <span style="color:red">-0.6%</span> | 285.3365 | <span style="color:grey">+0.0%</span> | 121.6395 | <span style="color:green">+2.3%</span> | 2.6371 | <span style="color:red">+0.6%</span> | 45505 |
| vertexattributes | Proxy Tuned | 379.8674 | <span style="color:red">-0.5%</span> | 284.8827 | <span style="color:red">-0.1%</span> | 118.9403 | <span style="color:grey">-0.0%</span> | 2.6325 | <span style="color:red">+0.5%</span> | 45585 |
| occlusionquery | Native | 996.9362 | — | 306.7605 | — | 193.6251 | — | 1.0031 | — | 119633 |
| occlusionquery | Proxy Default | 993.7274 | <span style="color:red">-0.3%</span> | 305.3326 | <span style="color:red">-0.5%</span> | 193.7635 | <span style="color:green">+0.1%</span> | 1.0063 | <span style="color:red">+0.3%</span> | 119248 |
| occlusionquery | Proxy Tuned | 994.6779 | <span style="color:red">-0.2%</span> | 307.2305 | <span style="color:green">+0.2%</span> | 196.9636 | <span style="color:green">+1.7%</span> | 1.0054 | <span style="color:red">+0.2%</span> | 119362 |
| dynamicuniformbuffer | Native | 969.3660 | — | 290.3054 | — | 177.1689 | — | 1.0316 | — | 116331 |
| dynamicuniformbuffer | Proxy Default | 970.8643 | <span style="color:green">+0.2%</span> | 291.9268 | <span style="color:green">+0.6%</span> | 184.7222 | <span style="color:green">+4.3%</span> | 1.0300 | <span style="color:green">-0.2%</span> | 116504 |
| dynamicuniformbuffer | Proxy Tuned | 969.1575 | <span style="color:grey">-0.0%</span> | 290.2838 | <span style="color:grey">-0.0%</span> | 181.1931 | <span style="color:green">+2.3%</span> | 1.0318 | <span style="color:grey">+0.0%</span> | 116301 |
| gltfscenerendering | Native | 390.6229 | — | 270.3218 | — | 114.1804 | — | 2.5600 | — | 46875 |
| gltfscenerendering | Proxy Default | 389.0369 | <span style="color:red">-0.4%</span> | 273.2301 | <span style="color:green">+1.1%</span> | 114.1451 | <span style="color:grey">-0.0%</span> | 2.5704 | <span style="color:red">+0.4%</span> | 46685 |
| gltfscenerendering | Proxy Tuned | 389.0084 | <span style="color:red">-0.4%</span> | 296.8073 | <span style="color:green">+9.8%</span> | 165.5369 | <span style="color:green">+45.0%</span> | 2.5706 | <span style="color:red">+0.4%</span> | 46682 |
| specializationconstants | Native | 832.5863 | — | 138.1377 | — | 94.9356 | — | 1.2011 | — | 99911 |
| specializationconstants | Proxy Default | 849.5284 | <span style="color:green">+2.0%</span> | 145.2943 | <span style="color:green">+5.2%</span> | 96.0122 | <span style="color:green">+1.1%</span> | 1.1771 | <span style="color:green">-2.0%</span> | 101944 |
| specializationconstants | Proxy Tuned | 863.5122 | <span style="color:green">+3.7%</span> | 153.9810 | <span style="color:green">+11.5%</span> | 100.0338 | <span style="color:green">+5.4%</span> | 1.1581 | <span style="color:green">-3.6%</span> | 103622 |
| triangle | Native | 985.6511 | — | 298.1543 | — | 189.0799 | — | 1.0146 | — | 118279 |
| triangle | Proxy Default | 984.2080 | <span style="color:red">-0.1%</span> | 299.0201 | <span style="color:green">+0.3%</span> | 194.7835 | <span style="color:green">+3.0%</span> | 1.0160 | <span style="color:red">+0.1%</span> | 118105 |
| triangle | Proxy Tuned | 977.7813 | <span style="color:red">-0.8%</span> | 252.7386 | <span style="color:red">-15.2%</span> | 91.2046 | <span style="color:red">-51.8%</span> | 1.0227 | <span style="color:red">+0.8%</span> | 117334 |
| inputattachments | Native | 989.8794 | — | 283.1145 | — | 163.1664 | — | 1.0102 | — | 118786 |
| inputattachments | Proxy Default | 991.0289 | <span style="color:green">+0.1%</span> | 282.0915 | <span style="color:red">-0.4%</span> | 164.3669 | <span style="color:green">+0.7%</span> | 1.0091 | <span style="color:green">-0.1%</span> | 118924 |
| inputattachments | Proxy Tuned | 990.2343 | <span style="color:grey">+0.0%</span> | 282.8160 | <span style="color:red">-0.1%</span> | 165.9998 | <span style="color:green">+1.7%</span> | 1.0099 | <span style="color:grey">-0.0%</span> | 118829 |
| subpasses | Native | 64.3154 | — | 24.8848 | — | 4.0404 | — | 15.5484 | — | 7718 |
| subpasses | Proxy Default | 64.7588 | <span style="color:green">+0.7%</span> | 61.3105 | <span style="color:green">+146.4%</span> | 60.2783 | <span style="color:green">+1391.9%</span> | 15.4419 | <span style="color:green">-0.7%</span> | 7772 |
| subpasses | Proxy Tuned | 64.6062 | <span style="color:green">+0.5%</span> | 51.2953 | <span style="color:green">+106.1%</span> | 21.1619 | <span style="color:green">+423.8%</span> | 15.4784 | <span style="color:green">-0.5%</span> | 7753 |
| bloom | Native | 950.3196 | — | 264.6344 | — | 108.2230 | — | 1.0523 | — | 114039 |
| bloom | Proxy Default | 945.7696 | <span style="color:red">-0.5%</span> | 255.8604 | <span style="color:red">-3.3%</span> | 103.2198 | <span style="color:red">-4.6%</span> | 1.0573 | <span style="color:red">+0.5%</span> | 113493 |
| bloom | Proxy Tuned | 950.4831 | <span style="color:grey">+0.0%</span> | 291.1638 | <span style="color:green">+10.0%</span> | 173.3303 | <span style="color:green">+60.2%</span> | 1.0521 | <span style="color:grey">-0.0%</span> | 114058 |
| sphericalenvmapping | Native | 832.9280 | — | 138.5983 | — | 95.3158 | — | 1.2006 | — | 99952 |
| sphericalenvmapping | Proxy Default | 833.2995 | <span style="color:grey">+0.0%</span> | 141.1605 | <span style="color:green">+1.8%</span> | 107.0594 | <span style="color:green">+12.3%</span> | 1.2000 | <span style="color:grey">-0.0%</span> | 100001 |
| sphericalenvmapping | Proxy Tuned | 835.6642 | <span style="color:green">+0.3%</span> | 141.4587 | <span style="color:green">+2.1%</span> | 98.1489 | <span style="color:green">+3.0%</span> | 1.1967 | <span style="color:green">-0.3%</span> | 100280 |
| textoverlay | Native | 978.1066 | — | 299.7962 | — | 191.3924 | — | 1.0224 | — | 117373 |
| textoverlay | Proxy Default | 978.4783 | <span style="color:grey">+0.0%</span> | 297.5144 | <span style="color:red">-0.8%</span> | 196.2424 | <span style="color:green">+2.5%</span> | 1.0220 | <span style="color:grey">-0.0%</span> | 117418 |
| textoverlay | Proxy Tuned | 975.6260 | <span style="color:red">-0.3%</span> | 297.5819 | <span style="color:red">-0.7%</span> | 192.2197 | <span style="color:green">+0.4%</span> | 1.0250 | <span style="color:red">+0.3%</span> | 117076 |
| texturemipmapgen | Native | 850.6660 | — | 162.7615 | — | 87.4229 | — | 1.1755 | — | 102080 |
| texturemipmapgen | Proxy Default | 1040.0487 | <span style="color:green">+22.3%</span> | 240.9630 | <span style="color:green">+48.0%</span> | 152.9670 | <span style="color:green">+75.0%</span> | 0.9615 | <span style="color:green">-18.2%</span> | 124806 |
| texturemipmapgen | Proxy Tuned | 1038.7294 | <span style="color:green">+22.1%</span> | 242.4698 | <span style="color:green">+49.0%</span> | 154.6913 | <span style="color:green">+76.9%</span> | 0.9627 | <span style="color:green">-18.1%</span> | 124648 |
| dynamicstate | Native | 1015.1641 | — | 234.9408 | — | 149.7259 | — | 0.9851 | — | 121820 |
| dynamicstate | Proxy Default | 1015.0664 | <span style="color:grey">-0.0%</span> | 235.4012 | <span style="color:green">+0.2%</span> | 150.4824 | <span style="color:green">+0.5%</span> | 0.9852 | <span style="color:grey">+0.0%</span> | 121809 |
| dynamicstate | Proxy Tuned | 1015.8905 | <span style="color:green">+0.1%</span> | 235.3302 | <span style="color:green">+0.2%</span> | 149.6075 | <span style="color:red">-0.1%</span> | 0.9844 | <span style="color:green">-0.1%</span> | 121907 |
| dynamicrenderingmultisampling | Native | 436.7034 | — | 344.7886 | — | 178.6690 | — | 2.2899 | — | 52405 |
| dynamicrenderingmultisampling | Proxy Default | 436.4477 | <span style="color:red">-0.1%</span> | 338.2768 | <span style="color:red">-1.9%</span> | 164.5708 | <span style="color:red">-7.9%</span> | 2.2912 | <span style="color:red">+0.1%</span> | 52374 |
| dynamicrenderingmultisampling | Proxy Tuned | 436.4058 | <span style="color:red">-0.1%</span> | 336.3078 | <span style="color:red">-2.5%</span> | 158.8151 | <span style="color:red">-11.1%</span> | 2.2914 | <span style="color:red">+0.1%</span> | 52369 |
| conditionalrender | Native | 1001.1363 | — | 243.8228 | — | 157.6246 | — | 0.9989 | — | 120137 |
| conditionalrender | Proxy Default | 993.1459 | <span style="color:red">-0.8%</span> | 242.9223 | <span style="color:red">-0.4%</span> | 152.7456 | <span style="color:red">-3.1%</span> | 1.0069 | <span style="color:red">+0.8%</span> | 119178 |
| conditionalrender | Proxy Tuned | 998.6962 | <span style="color:red">-0.2%</span> | 251.9934 | <span style="color:green">+3.4%</span> | 181.8954 | <span style="color:green">+15.4%</span> | 1.0013 | <span style="color:red">+0.2%</span> | 119844 |
| descriptorheap | Native | NA | — | NA | — | NA | — | NA | — | NA |
| descriptorheap | Proxy Default | NA | — | NA | — | NA | — | NA | — | NA |
| descriptorheap | Proxy Tuned | NA | — | NA | — | NA | — | NA | — | NA |
| radialblur | Native | 539.4013 | — | 389.8748 | — | 150.5873 | — | 1.8539 | — | 64729 |
| radialblur | Proxy Default | 535.9089 | <span style="color:red">-0.6%</span> | 386.7558 | <span style="color:red">-0.8%</span> | 147.8901 | <span style="color:red">-1.8%</span> | 1.8660 | <span style="color:red">+0.7%</span> | 64310 |
| radialblur | Proxy Tuned | 535.4774 | <span style="color:red">-0.7%</span> | 392.0706 | <span style="color:green">+0.6%</span> | 151.9361 | <span style="color:green">+0.9%</span> | 1.8675 | <span style="color:red">+0.7%</span> | 64258 |
| raytracingcallable | Native | NA | — | NA | — | NA | — | NA | — | NA |
| raytracingcallable | Proxy Default | NA | — | NA | — | NA | — | NA | — | NA |
| raytracingcallable | Proxy Tuned | NA | — | NA | — | NA | — | NA | — | NA |
| descriptorindexing | Native | 970.3961 | — | 292.2499 | — | 184.6482 | — | 1.0305 | — | 116448 |
| descriptorindexing | Proxy Default | 971.2396 | <span style="color:green">+0.1%</span> | 293.4010 | <span style="color:green">+0.4%</span> | 188.6984 | <span style="color:green">+2.2%</span> | 1.0296 | <span style="color:green">-0.1%</span> | 116549 |
| descriptorindexing | Proxy Tuned | 970.6450 | <span style="color:grey">+0.0%</span> | 291.4599 | <span style="color:red">-0.3%</span> | 184.8106 | <span style="color:green">+0.1%</span> | 1.0302 | <span style="color:grey">-0.0%</span> | 116478 |
| trianglevulkan13 | Native | 984.2999 | — | 298.6137 | — | 192.4561 | — | 1.0160 | — | 118118 |
| trianglevulkan13 | Proxy Default | 988.4454 | <span style="color:green">+0.4%</span> | 300.9537 | <span style="color:green">+0.8%</span> | 196.6853 | <span style="color:green">+2.2%</span> | 1.0117 | <span style="color:green">-0.4%</span> | 118614 |
| trianglevulkan13 | Proxy Tuned | 987.7415 | <span style="color:green">+0.3%</span> | 301.3638 | <span style="color:green">+0.9%</span> | 195.6928 | <span style="color:green">+1.7%</span> | 1.0124 | <span style="color:green">-0.4%</span> | 118529 |
| rayquery | Native | NA | — | NA | — | NA | — | NA | — | NA |
| rayquery | Proxy Default | NA | — | NA | — | NA | — | NA | — | NA |
| rayquery | Proxy Tuned | NA | — | NA | — | NA | — | NA | — | NA |
| raytracingsbtdata | Native | NA | — | NA | — | NA | — | NA | — | NA |
| raytracingsbtdata | Proxy Default | NA | — | NA | — | NA | — | NA | — | NA |
| raytracingsbtdata | Proxy Tuned | NA | — | NA | — | NA | — | NA | — | NA |
| shaderobjects | Native | 1057.5390 | — | 241.3074 | — | 155.1833 | — | 0.9456 | — | 126907 |
| shaderobjects | Proxy Default | 1056.2745 | <span style="color:red">-0.1%</span> | 242.3755 | <span style="color:green">+0.4%</span> | 151.4994 | <span style="color:red">-2.4%</span> | 0.9467 | <span style="color:red">+0.1%</span> | 126753 |
| shaderobjects | Proxy Tuned | 1058.2567 | <span style="color:green">+0.1%</span> | 244.4430 | <span style="color:green">+1.3%</span> | 157.3079 | <span style="color:green">+1.4%</span> | 0.9450 | <span style="color:green">-0.1%</span> | 126991 |
| pipelines | Native | 1034.0733 | — | 241.4193 | — | 155.1449 | — | 0.9670 | — | 124089 |
| pipelines | Proxy Default | 1041.6192 | <span style="color:green">+0.7%</span> | 240.5988 | <span style="color:red">-0.3%</span> | 153.0842 | <span style="color:red">-1.3%</span> | 0.9600 | <span style="color:green">-0.7%</span> | 124995 |
| pipelines | Proxy Tuned | 1038.3517 | <span style="color:green">+0.4%</span> | 241.5214 | <span style="color:grey">+0.0%</span> | 155.7491 | <span style="color:green">+0.4%</span> | 0.9631 | <span style="color:green">-0.4%</span> | 124603 |
| computeraytracing | Native | 26.7992 | — | 25.5867 | — | 25.0760 | — | 37.3146 | — | 3216 |
| computeraytracing | Proxy Default | 26.7941 | <span style="color:grey">-0.0%</span> | 25.5993 | <span style="color:grey">+0.0%</span> | 25.0734 | <span style="color:grey">-0.0%</span> | 37.3216 | <span style="color:grey">+0.0%</span> | 3216 |
| computeraytracing | Proxy Tuned | 26.7949 | <span style="color:grey">-0.0%</span> | 25.5939 | <span style="color:grey">+0.0%</span> | 25.0908 | <span style="color:green">+0.1%</span> | 37.3205 | <span style="color:grey">+0.0%</span> | 3216 |
| computeheadless | Native | NA | — | NA | — | NA | — | NA | — | NA |
| computeheadless | Proxy Default | NA | — | NA | — | NA | — | NA | — | NA |
| computeheadless | Proxy Tuned | NA | — | NA | — | NA | — | NA | — | NA |
| raytracingtextures | Native | NA | — | NA | — | NA | — | NA | — | NA |
| raytracingtextures | Proxy Default | NA | — | NA | — | NA | — | NA | — | NA |
| raytracingtextures | Proxy Tuned | NA | — | NA | — | NA | — | NA | — | NA |
| pbrbasic | Native | 1016.1801 | — | 270.5096 | — | 160.5253 | — | 0.9841 | — | 121942 |
| pbrbasic | Proxy Default | 1022.4013 | <span style="color:green">+0.6%</span> | 271.9064 | <span style="color:green">+0.5%</span> | 159.3997 | <span style="color:red">-0.7%</span> | 0.9781 | <span style="color:green">-0.6%</span> | 122689 |
| pbrbasic | Proxy Tuned | 1021.2574 | <span style="color:green">+0.5%</span> | 271.3753 | <span style="color:green">+0.3%</span> | 164.1324 | <span style="color:green">+2.2%</span> | 0.9792 | <span style="color:green">-0.5%</span> | 122551 |
| deferredmultisampling | Native | 22.4138 | — | 22.1028 | — | 22.0287 | — | 44.6154 | — | 2690 |
| deferredmultisampling | Proxy Default | 22.4075 | <span style="color:grey">-0.0%</span> | 22.0570 | <span style="color:red">-0.2%</span> | 21.9933 | <span style="color:red">-0.2%</span> | 44.6279 | <span style="color:grey">+0.0%</span> | 2689 |
| deferredmultisampling | Proxy Tuned | 22.3616 | <span style="color:red">-0.2%</span> | 21.9004 | <span style="color:red">-0.9%</span> | 21.6506 | <span style="color:red">-1.7%</span> | 44.7196 | <span style="color:red">+0.2%</span> | 2684 |
| shadowmappingcascade | Native | 52.5397 | — | 51.2116 | — | 50.5930 | — | 19.0332 | — | 6305 |
| shadowmappingcascade | Proxy Default | 52.5190 | <span style="color:grey">-0.0%</span> | 51.2411 | <span style="color:green">+0.1%</span> | 50.7273 | <span style="color:green">+0.3%</span> | 19.0407 | <span style="color:grey">+0.0%</span> | 6303 |
| shadowmappingcascade | Proxy Tuned | 52.5195 | <span style="color:grey">-0.0%</span> | 50.7407 | <span style="color:red">-0.9%</span> | 50.3106 | <span style="color:red">-0.6%</span> | 19.0406 | <span style="color:grey">+0.0%</span> | 6303 |
| shadowmapping | Native | 956.2552 | — | 310.3320 | — | 172.1960 | — | 1.0457 | — | 114752 |
| shadowmapping | Proxy Default | 954.9603 | <span style="color:red">-0.1%</span> | 311.2210 | <span style="color:green">+0.3%</span> | 172.0137 | <span style="color:red">-0.1%</span> | 1.0472 | <span style="color:red">+0.1%</span> | 114596 |
| shadowmapping | Proxy Tuned | 955.9370 | <span style="color:grey">-0.0%</span> | 311.4523 | <span style="color:green">+0.4%</span> | 175.8161 | <span style="color:green">+2.1%</span> | 1.0461 | <span style="color:grey">+0.0%</span> | 114713 |
| oit | Native | 111.4737 | — | 106.7070 | — | 103.5915 | — | 8.9707 | — | 13377 |
| oit | Proxy Default | 111.3205 | <span style="color:red">-0.1%</span> | 106.6287 | <span style="color:red">-0.1%</span> | 103.4273 | <span style="color:red">-0.2%</span> | 8.9831 | <span style="color:red">+0.1%</span> | 13359 |
| oit | Proxy Tuned | 111.3221 | <span style="color:red">-0.1%</span> | 106.4905 | <span style="color:red">-0.2%</span> | 103.4937 | <span style="color:red">-0.1%</span> | 8.9829 | <span style="color:red">+0.1%</span> | 13359 |
| renderheadless | Native | NA | — | NA | — | NA | — | NA | — | NA |
| renderheadless | Proxy Default | NA | — | NA | — | NA | — | NA | — | NA |
| renderheadless | Proxy Tuned | NA | — | NA | — | NA | — | NA | — | NA |
| descriptorbuffer | Native | 961.6387 | — | 280.7496 | — | 174.8607 | — | 1.0399 | — | 115397 |
| descriptorbuffer | Proxy Default | NA | — | NA | — | NA | — | NA | — | NA |
| descriptorbuffer | Proxy Tuned | NA | — | NA | — | NA | — | NA | — | NA |
| pushdescriptors | Native | 971.7021 | — | 248.1886 | — | 89.3167 | — | 1.0291 | — | 116605 |
| pushdescriptors | Proxy Default | NA | — | NA | — | NA | — | NA | — | NA |
| pushdescriptors | Proxy Tuned | NA | — | NA | — | NA | — | NA | — | NA |
| hostimagecopy | Native | NA | — | NA | — | NA | — | NA | — | NA |
| hostimagecopy | Proxy Default | NA | — | NA | — | NA | — | NA | — | NA |
| hostimagecopy | Proxy Tuned | NA | — | NA | — | NA | — | NA | — | NA |
| computenbody | Native | 151.5159 | — | 102.2375 | — | 85.2250 | — | 6.6000 | — | 18183 |
| computenbody | Proxy Default | 149.4645 | <span style="color:red">-1.4%</span> | 101.5733 | <span style="color:red">-0.6%</span> | 83.4232 | <span style="color:red">-2.1%</span> | 6.6906 | <span style="color:red">+1.4%</span> | 17937 |
| computenbody | Proxy Tuned | 148.0209 | <span style="color:red">-2.3%</span> | 102.2435 | <span style="color:grey">+0.0%</span> | 98.5767 | <span style="color:green">+15.7%</span> | 6.7558 | <span style="color:red">+2.4%</span> | 17763 |
| terraintessellation | Native | 432.8884 | — | 273.3351 | — | 88.4880 | — | 2.3101 | — | 51947 |
| terraintessellation | Proxy Default | 435.6777 | <span style="color:green">+0.6%</span> | 311.4536 | <span style="color:green">+13.9%</span> | 144.6547 | <span style="color:green">+63.5%</span> | 2.2953 | <span style="color:green">-0.6%</span> | 52282 |
| terraintessellation | Proxy Tuned | 433.4346 | <span style="color:green">+0.1%</span> | 312.5392 | <span style="color:green">+14.3%</span> | 152.4620 | <span style="color:green">+72.3%</span> | 2.3072 | <span style="color:green">-0.1%</span> | 52013 |
| raytracingintersection | Native | NA | — | NA | — | NA | — | NA | — | NA |
| raytracingintersection | Proxy Default | NA | — | NA | — | NA | — | NA | — | NA |
| raytracingintersection | Proxy Tuned | NA | — | NA | — | NA | — | NA | — | NA |
| texturecubemap | Native | 795.9157 | — | 148.3848 | — | 101.2583 | — | 1.2564 | — | 95511 |
| texturecubemap | Proxy Default | 783.2348 | <span style="color:red">-1.6%</span> | 145.2939 | <span style="color:red">-2.1%</span> | 103.2160 | <span style="color:green">+1.9%</span> | 1.2768 | <span style="color:red">+1.6%</span> | 93989 |
| texturecubemap | Proxy Tuned | 793.5314 | <span style="color:red">-0.3%</span> | 151.4385 | <span style="color:green">+2.1%</span> | 99.2479 | <span style="color:red">-2.0%</span> | 1.2602 | <span style="color:red">+0.3%</span> | 95224 |
| multithreading | Native | 330.8490 | — | 148.7997 | — | 68.0782 | — | 3.0225 | — | 39702 |
| multithreading | Proxy Default | 332.3885 | <span style="color:green">+0.5%</span> | 157.0629 | <span style="color:green">+5.6%</span> | 61.3176 | <span style="color:red">-9.9%</span> | 3.0085 | <span style="color:green">-0.5%</span> | 39888 |
| multithreading | Proxy Tuned | 334.9506 | <span style="color:green">+1.2%</span> | 197.4683 | <span style="color:green">+32.7%</span> | 132.4635 | <span style="color:green">+94.6%</span> | 2.9855 | <span style="color:green">-1.2%</span> | 40195 |
| tessellation | Native | 984.5282 | — | 296.0252 | — | 178.0610 | — | 1.0157 | — | 118146 |
| tessellation | Proxy Default | 980.3089 | <span style="color:red">-0.4%</span> | 291.6080 | <span style="color:red">-1.5%</span> | 169.3856 | <span style="color:red">-4.9%</span> | 1.0201 | <span style="color:red">+0.4%</span> | 117639 |
| tessellation | Proxy Tuned | 981.1700 | <span style="color:red">-0.3%</span> | 291.5200 | <span style="color:red">-1.5%</span> | 170.6928 | <span style="color:red">-4.1%</span> | 1.0192 | <span style="color:red">+0.3%</span> | 117741 |
| pipelinestatistics | Native | 1070.1972 | — | 302.0942 | — | 180.4337 | — | 0.9344 | — | 128424 |
| pipelinestatistics | Proxy Default | 1065.3299 | <span style="color:red">-0.5%</span> | 300.7919 | <span style="color:red">-0.4%</span> | 182.1016 | <span style="color:green">+0.9%</span> | 0.9387 | <span style="color:red">+0.5%</span> | 127842 |
| pipelinestatistics | Proxy Tuned | 1059.2576 | <span style="color:red">-1.0%</span> | 299.9247 | <span style="color:red">-0.7%</span> | 179.9250 | <span style="color:red">-0.3%</span> | 0.9441 | <span style="color:red">+1.0%</span> | 127111 |
| graphicspipelinelibrary | Native | 980.6009 | — | 290.6585 | — | 180.1580 | — | 1.0198 | — | 117674 |
| graphicspipelinelibrary | Proxy Default | 979.8726 | <span style="color:red">-0.1%</span> | 289.5990 | <span style="color:red">-0.4%</span> | 180.8666 | <span style="color:green">+0.4%</span> | 1.0205 | <span style="color:red">+0.1%</span> | 117585 |
| graphicspipelinelibrary | Proxy Tuned | 979.7150 | <span style="color:red">-0.1%</span> | 290.4613 | <span style="color:red">-0.1%</span> | 180.6245 | <span style="color:green">+0.3%</span> | 1.0207 | <span style="color:red">+0.1%</span> | 117566 |
| hdr | Native | 112.5678 | — | 107.6255 | — | 101.5270 | — | 8.8835 | — | 13509 |
| hdr | Proxy Default | 112.5669 | <span style="color:grey">-0.0%</span> | 107.7034 | <span style="color:green">+0.1%</span> | 102.2612 | <span style="color:green">+0.7%</span> | 8.8836 | <span style="color:grey">+0.0%</span> | 13509 |
| hdr | Proxy Tuned | 112.5601 | <span style="color:grey">-0.0%</span> | 108.0346 | <span style="color:green">+0.4%</span> | 103.5812 | <span style="color:green">+2.0%</span> | 8.8841 | <span style="color:grey">+0.0%</span> | 13508 |
| computeparticles | Native | 282.5660 | — | 219.6793 | — | 99.2228 | — | 3.5390 | — | 33908 |
| computeparticles | Proxy Default | 281.9979 | <span style="color:red">-0.2%</span> | 220.5695 | <span style="color:green">+0.4%</span> | 101.8688 | <span style="color:green">+2.7%</span> | 3.5461 | <span style="color:red">+0.2%</span> | 33840 |
| computeparticles | Proxy Tuned | 281.4856 | <span style="color:red">-0.4%</span> | 218.3486 | <span style="color:red">-0.6%</span> | 101.4263 | <span style="color:green">+2.2%</span> | 3.5526 | <span style="color:red">+0.4%</span> | 33779 |
| vulkanscene | Native | 997.1612 | — | 279.6084 | — | 156.0763 | — | 1.0028 | — | 119660 |
| vulkanscene | Proxy Default | 974.6880 | <span style="color:red">-2.3%</span> | 199.0911 | <span style="color:red">-28.8%</span> | 58.9252 | <span style="color:red">-62.2%</span> | 1.0260 | <span style="color:red">+2.3%</span> | 116963 |
| vulkanscene | Proxy Tuned | 1006.2967 | <span style="color:green">+0.9%</span> | 283.2521 | <span style="color:green">+1.3%</span> | 182.0771 | <span style="color:green">+16.7%</span> | 0.9937 | <span style="color:green">-0.9%</span> | 120756 |
| imgui | Native | 1001.5316 | — | 283.1672 | — | 158.9799 | — | 0.9985 | — | 120184 |
| imgui | Proxy Default | 1003.2414 | <span style="color:green">+0.2%</span> | 283.4484 | <span style="color:green">+0.1%</span> | 160.2414 | <span style="color:green">+0.8%</span> | 0.9968 | <span style="color:green">-0.2%</span> | 120389 |
| imgui | Proxy Tuned | 1002.3482 | <span style="color:green">+0.1%</span> | 280.0204 | <span style="color:red">-1.1%</span> | 154.9516 | <span style="color:red">-2.5%</span> | 0.9977 | <span style="color:green">-0.1%</span> | 120282 |
| conservativeraster | Native | NA | — | NA | — | NA | — | NA | — | NA |
| conservativeraster | Proxy Default | NA | — | NA | — | NA | — | NA | — | NA |
| conservativeraster | Proxy Tuned | NA | — | NA | — | NA | — | NA | — | NA |
| geometryshader | Native | 976.6447 | — | 293.6751 | — | 182.0721 | — | 1.0239 | — | 117198 |
| geometryshader | Proxy Default | 976.2449 | <span style="color:grey">-0.0%</span> | 291.1749 | <span style="color:red">-0.9%</span> | 178.1388 | <span style="color:red">-2.2%</span> | 1.0243 | <span style="color:grey">+0.0%</span> | 117150 |
| geometryshader | Proxy Tuned | 976.6298 | <span style="color:grey">-0.0%</span> | 291.0359 | <span style="color:red">-0.9%</span> | 177.8039 | <span style="color:red">-2.3%</span> | 1.0239 | <span style="color:grey">+0.0%</span> | 117196 |
| gears | Native | 969.8442 | — | 290.8585 | — | 178.4179 | — | 1.0311 | — | 116382 |
| gears | Proxy Default | 970.5221 | <span style="color:green">+0.1%</span> | 292.8569 | <span style="color:green">+0.7%</span> | 185.0208 | <span style="color:green">+3.7%</span> | 1.0304 | <span style="color:green">-0.1%</span> | 116463 |
| gears | Proxy Tuned | 970.5871 | <span style="color:green">+0.1%</span> | 292.0243 | <span style="color:green">+0.4%</span> | 185.2675 | <span style="color:green">+3.8%</span> | 1.0303 | <span style="color:green">-0.1%</span> | 116471 |
| pbribl | Native | 1037.0285 | — | 234.1270 | — | 150.6607 | — | 0.9643 | — | 124444 |
| pbribl | Proxy Default | 1030.8033 | <span style="color:red">-0.6%</span> | 233.4175 | <span style="color:red">-0.3%</span> | 149.3581 | <span style="color:red">-0.9%</span> | 0.9701 | <span style="color:red">+0.6%</span> | 123697 |
| pbribl | Proxy Tuned | 1031.1216 | <span style="color:red">-0.6%</span> | 237.3700 | <span style="color:green">+1.4%</span> | 150.1426 | <span style="color:red">-0.3%</span> | 0.9698 | <span style="color:red">+0.6%</span> | 123736 |
| deferred | Native | 180.5636 | — | 166.4694 | — | 146.8793 | — | 5.5382 | — | 21668 |
| deferred | Proxy Default | 180.6107 | <span style="color:grey">+0.0%</span> | 166.3554 | <span style="color:red">-0.1%</span> | 142.5380 | <span style="color:red">-3.0%</span> | 5.5368 | <span style="color:grey">-0.0%</span> | 21674 |
| deferred | Proxy Tuned | 180.6113 | <span style="color:grey">+0.0%</span> | 166.5875 | <span style="color:green">+0.1%</span> | 152.8680 | <span style="color:green">+4.1%</span> | 5.5368 | <span style="color:grey">-0.0%</span> | 21674 |
| bufferdeviceaddress | Native | 956.0906 | — | 282.0683 | — | 177.6554 | — | 1.0459 | — | 114731 |
| bufferdeviceaddress | Proxy Default | 956.1891 | <span style="color:grey">+0.0%</span> | 283.0251 | <span style="color:green">+0.3%</span> | 182.2026 | <span style="color:green">+2.6%</span> | 1.0458 | <span style="color:grey">-0.0%</span> | 114743 |
| bufferdeviceaddress | Proxy Tuned | 956.3583 | <span style="color:grey">+0.0%</span> | 283.6659 | <span style="color:green">+0.6%</span> | 185.5045 | <span style="color:green">+4.4%</span> | 1.0456 | <span style="color:grey">-0.0%</span> | 114765 |
| particlesystem | Native | 1014.5710 | — | 253.3561 | — | 158.1613 | — | 0.9856 | — | 121749 |
| particlesystem | Proxy Default | 1015.1797 | <span style="color:green">+0.1%</span> | 261.1572 | <span style="color:green">+3.1%</span> | 153.4232 | <span style="color:red">-3.0%</span> | 0.9850 | <span style="color:green">-0.1%</span> | 121824 |
| particlesystem | Proxy Tuned | 1017.9262 | <span style="color:green">+0.3%</span> | 261.1430 | <span style="color:green">+3.1%</span> | 156.8824 | <span style="color:red">-0.8%</span> | 0.9824 | <span style="color:green">-0.3%</span> | 122152 |
| raytracingpositionfetch | Native | NA | — | NA | — | NA | — | NA | — | NA |
| raytracingpositionfetch | Proxy Default | NA | — | NA | — | NA | — | NA | — | NA |
| raytracingpositionfetch | Proxy Tuned | NA | — | NA | — | NA | — | NA | — | NA |
| multisamplingalphatocoverage | Native | NA | — | NA | — | NA | — | NA | — | NA |
| multisamplingalphatocoverage | Proxy Default | NA | — | NA | — | NA | — | NA | — | NA |
| multisamplingalphatocoverage | Proxy Tuned | NA | — | NA | — | NA | — | NA | — | NA |
| ssao | Native | 54.6738 | — | 51.6819 | — | 50.9692 | — | 18.2903 | — | 6561 |
| ssao | Proxy Default | 54.1464 | <span style="color:red">-1.0%</span> | 51.0165 | <span style="color:red">-1.3%</span> | 50.4015 | <span style="color:red">-1.1%</span> | 18.4684 | <span style="color:red">+1.0%</span> | 6498 |
| ssao | Proxy Tuned | 54.1795 | <span style="color:red">-0.9%</span> | 50.9718 | <span style="color:red">-1.4%</span> | 50.5005 | <span style="color:red">-0.9%</span> | 18.4572 | <span style="color:red">+0.9%</span> | 6502 |
| parallaxmapping | Native | 847.2131 | — | 350.3129 | — | 188.3411 | — | 1.1803 | — | 101666 |
| parallaxmapping | Proxy Default | 845.8810 | <span style="color:red">-0.2%</span> | 361.7781 | <span style="color:green">+3.3%</span> | 221.6285 | <span style="color:green">+17.7%</span> | 1.1822 | <span style="color:red">+0.2%</span> | 101506 |
| parallaxmapping | Proxy Tuned | 844.5692 | <span style="color:red">-0.3%</span> | 363.2118 | <span style="color:green">+3.7%</span> | 194.9857 | <span style="color:green">+3.5%</span> | 1.1840 | <span style="color:red">+0.3%</span> | 101349 |
| gltfskinning | Native | 981.9041 | — | 291.9583 | — | 183.1979 | — | 1.0184 | — | 117829 |
| gltfskinning | Proxy Default | 980.6897 | <span style="color:red">-0.1%</span> | 291.3208 | <span style="color:red">-0.2%</span> | 182.3033 | <span style="color:red">-0.5%</span> | 1.0197 | <span style="color:red">+0.1%</span> | 117683 |
| gltfskinning | Proxy Tuned | 982.5968 | <span style="color:green">+0.1%</span> | 290.0189 | <span style="color:red">-0.7%</span> | 180.3127 | <span style="color:red">-1.6%</span> | 1.0177 | <span style="color:green">-0.1%</span> | 117912 |
| stencilbuffer | Native | 890.9018 | — | 230.7703 | — | 128.8765 | — | 1.1225 | — | 106909 |
| stencilbuffer | Proxy Default | 859.3672 | <span style="color:red">-3.5%</span> | 196.7949 | <span style="color:red">-14.7%</span> | 114.7253 | <span style="color:red">-11.0%</span> | 1.1636 | <span style="color:red">+3.7%</span> | 103128 |
| stencilbuffer | Proxy Tuned | 870.8452 | <span style="color:red">-2.3%</span> | 161.7802 | <span style="color:red">-29.9%</span> | 38.9155 | <span style="color:red">-69.8%</span> | 1.1483 | <span style="color:red">+2.3%</span> | 104502 |
| indirectdraw | Native | 35.4429 | — | 34.6244 | — | 34.4303 | — | 28.2144 | — | 4254 |
| indirectdraw | Proxy Default | 35.1491 | <span style="color:red">-0.8%</span> | 34.2794 | <span style="color:red">-1.0%</span> | 34.0973 | <span style="color:red">-1.0%</span> | 28.4503 | <span style="color:red">+0.8%</span> | 4218 |
| indirectdraw | Proxy Tuned | 35.1480 | <span style="color:red">-0.8%</span> | 34.2970 | <span style="color:red">-0.9%</span> | 34.1391 | <span style="color:red">-0.8%</span> | 28.4511 | <span style="color:red">+0.8%</span> | 4218 |
| debugprintf | Native | 1009.9371 | — | 240.2602 | — | 153.3937 | — | 0.9902 | — | 121193 |
| debugprintf | Proxy Default | 1011.1080 | <span style="color:green">+0.1%</span> | 238.1891 | <span style="color:red">-0.9%</span> | 152.0240 | <span style="color:red">-0.9%</span> | 0.9890 | <span style="color:green">-0.1%</span> | 121334 |
| debugprintf | Proxy Tuned | 1011.3230 | <span style="color:green">+0.1%</span> | 240.0796 | <span style="color:red">-0.1%</span> | 151.8321 | <span style="color:red">-1.0%</span> | 0.9888 | <span style="color:green">-0.1%</span> | 121359 |
| computeshader | Native | 968.9955 | — | 296.8206 | — | 187.0443 | — | 1.0320 | — | 116280 |
| computeshader | Proxy Default | 970.0103 | <span style="color:green">+0.1%</span> | 305.3076 | <span style="color:green">+2.9%</span> | 225.5444 | <span style="color:green">+20.6%</span> | 1.0309 | <span style="color:green">-0.1%</span> | 116402 |
| computeshader | Proxy Tuned | 968.4023 | <span style="color:red">-0.1%</span> | 296.7846 | <span style="color:grey">-0.0%</span> | 185.5128 | <span style="color:red">-0.8%</span> | 1.0326 | <span style="color:red">+0.1%</span> | 116209 |
| descriptorsets | Native | 955.1085 | — | 281.6064 | — | 174.9251 | — | 1.0470 | — | 114614 |
| descriptorsets | Proxy Default | 954.1651 | <span style="color:red">-0.1%</span> | 280.8264 | <span style="color:red">-0.3%</span> | 177.0090 | <span style="color:green">+1.2%</span> | 1.0480 | <span style="color:red">+0.1%</span> | 114500 |
| descriptorsets | Proxy Tuned | 955.1379 | <span style="color:grey">+0.0%</span> | 281.4482 | <span style="color:red">-0.1%</span> | 179.3898 | <span style="color:green">+2.6%</span> | 1.0470 | <span style="color:grey">+0.0%</span> | 114617 |
| variablerateshading | Native | NA | — | NA | — | NA | — | NA | — | NA |
| variablerateshading | Proxy Default | NA | — | NA | — | NA | — | NA | — | NA |
| variablerateshading | Proxy Tuned | NA | — | NA | — | NA | — | NA | — | NA | 