; scn2ks test: line mode page (a line break is [p])
[macro name=mpmac][eval exp="f.m = mp.v" cond="mp.c"][wait time=&mp.t][endmacro]
*start|Page
[linemode mode=page]
【ai】Hello[wait time=100]world.
【ai】Ends with tag[wait time=10]
Next line.
[noer]
【pip】No erase.
【】Empty name.
【/Display only】Display only.
[r]
[ai voice=3]
【ai】Voiced 3.
【ai】Voiced 4.
[voice ai=ignore]
【ai】No voice.
【ai】Voiced 5.
[parse]
【ai】After parse.
[mpmac v=2 c=0 t=5]
[font size=30]Big[resetfont] text.
[jump storage=t_vn.ks target=*start cond="f.go"]
[jump target=&f.where]
*after
Unreachable title is inherited.
[if exp="f.a"]
[jump target=*start]
[else]
Else branch.
[endif]
After all.
*last|Last
[s]
*end|End
The end.
