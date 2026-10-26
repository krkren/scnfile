; scn2ks test: line mode line (a line break is [l])
*start|Line mode
[linemode mode=line]
【ai】First line, ends with l.
Second line, nameless.
Third.[p]
【pip】After a page.
[ai center]
Text after a tag.
【ai】Text then tag[wait time=10] then more.
Another[wait time=10]
[p]
[if exp="f.a"]
Inside.
[endif]
Last line.
