; scn2ks test: line mode vn (a line break is [l], an empty line [p])
[macro name=mpmac][eval exp="f.m = mp.v" cond="mp.c"][wait time=&mp.t][endmacro]
*start|VN mode
[linemode mode=vn]
【ai】
This is the first line.
And a second line that joins it.

[ai voice=1]
【ai】Voiced line.
Continues here.

A page with no name.
Line two of it.
[ai posleft]
After a tag.

【ai/Somebody】Display name.

[noer]
【pip】No erase next page.

【ai】Ruby: [ruby text=かんじ]漢字 and an inline wait[l] then more.
Text with 100% and [[brackets]] and a back\slash inside.

[voice ai=7 once]
【ai】Voice from a voice tag.

[voice pip=12]
【pip】Counter from a voice tag.
【pip】Counter again.

[nextvoice name=ai]
【pip】Two voices.

Line before an if.
[if exp="f.x == 1"]
【ai】In the if.

[elsif exp="f.x == 2"]
【pip】In the elsif.

[else]
Nobody.

[endif]
[emb exp="'emb text'"]
[eval exp="f.y = 1" cond="f.x > 0"]
[iscript]
f.z = 3;
	f.w = "tab";
[endscript]
[wait time=&f.y*10]
[mpmac v=5 c=1 t=100]
[mpmac v=1 c=1 t=1 cond="f.q"]
【ai】After the script.

[jump target=*second cond="f.skip"]
[jump target=*second]

*second|Second
【ai】The second scene.

[seladd text="Choice A" target=*start eval="f.a"]
[seladd text="Choice B" storage=t_page.ks target=*start exp="f.b = 1"]
[select]
