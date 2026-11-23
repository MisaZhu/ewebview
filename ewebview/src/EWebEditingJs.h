/* Selection / Range / contenteditable, implemented in page script.
 *
 * The DOM bridge already exposes everything an editing model needs (child
 * lists, text-node values, insertBefore/removeChild), so the Selection and
 * Range objects and the contenteditable editing commands live in a small JS
 * prelude instead of several hundred lines of native glue. initJsVm() runs it
 * once per VM, before any page script.
 *
 * The prelude leaves three things behind:
 *   - window.getSelection / document.getSelection / a working Range
 *     (document.createRange() used to return an inert stub);
 *   - Element.isContentEditable / contentEditable;
 *   - the global EWEB_CE_HOOK(host, op, text), which the engine calls for the
 *     focused editing host (see EWebEngine::jsEditableCommand). initJsVm() then
 *     moves that binding to the internal EWEB_CE_SLOT name, out of page reach.
 *
 * Hook ops: "focus" (make sure a caret exists inside the host), "text" (insert
 * `text` at the caret, replacing a selection), "back" / "del", "enter",
 * "left" / "right" / "home" / "end", "selall". It returns true when the host's
 * content changed, so the caller knows to fire `input`.
 *
 * TEXT-RUN GROUPS. The bridge coalesces consecutive layout text runs
 * (el_text/el_space) into ONE DOM text node: childNodes exposes the run-group
 * leader, nodeValue reads the whole group, but removeChild/nodeValue writes
 * only touch the leader run. Inserted text therefore vanishes into its
 * neighbour's group and its wrapper goes stale. Every mutation below works on
 * whole groups: a text edit REPLACES the group with one fresh text node
 * (rmGroup + insert), which - the group being maximal - has no text neighbours
 * and keeps its wrapper identity. Carets are carried as positions
 * {p: element, i: child index, o: offset inside a text child or -1 at a child
 * boundary} and re-resolved against live childNodes after each mutation, so no
 * stale wrapper is ever dereferenced. */
#ifndef EWEB_EDITING_JS_H
#define EWEB_EDITING_JS_H

#define EWEB_CE_HOOK "__eweb_ce"
#define EWEB_CE_SLOT "@@cehook"

static const char kEwebEditingPrelude[] = R"EWEBJS(
var __eweb_ce = (function(){
var doc = document;
function nlen(n){ var t = n.nodeType, v; if(t === 3 || t === 8){ v = n.nodeValue; return v == null ? 0 : v.length; } return n.childNodes.length; }
function nidx(n){ var p = n.parentNode, c, i; if(p == null) return -1; c = p.childNodes; for(i = 0; i < c.length; i++) if(c[i] === n) return i; return -1; }
function anc(a, n){ var g = 0; for(; n != null && g < 512; n = n.parentNode, g++) if(n === a) return true; return false; }
function top(n){ var g = 0; while(n != null && n.parentNode != null && g++ < 512) n = n.parentNode; return n; }
function path(n, o){ var p = [o], g = 0; while(n != null && n.parentNode != null && g++ < 512){ p.unshift(nidx(n)); n = n.parentNode; } return p; }
function cmp(n1, o1, n2, o2){
  if(n1 === n2) return o1 < o2 ? -1 : (o1 > o2 ? 1 : 0);
  var a = path(n1, o1), b = path(n2, o2), n = a.length < b.length ? a.length : b.length, i;
  for(i = 0; i < n; i++) if(a[i] !== b[i]) return a[i] < b[i] ? -1 : 1;
  return a.length < b.length ? -1 : (a.length > b.length ? 1 : 0);
}
function isCD(n){ return n.nodeType === 3 || n.nodeType === 8; }
function isTxt(n){ return n != null && n.nodeType === 3; }
function tlen(n){ var v = n.nodeValue; return v == null ? 0 : v.length; }

/* ---- text-run group primitives (see header comment) ---- */
function rmGroup(p, i){
  var c = p.childNodes, g = 0;
  while(i < c.length && isTxt(c[i]) && g++ < 4096){ p.removeChild(c[i]); c = p.childNodes; }
}
function replGroup(p, i, v){
  rmGroup(p, i);
  if(v === "") return null;
  var c = p.childNodes, nn = doc.createTextNode(v);
  if(i < c.length) p.insertBefore(nn, c[i]); else p.appendChild(nn);
  return nn;
}
function insAt(p, i, n){ var c = p.childNodes; if(i < c.length) p.insertBefore(n, c[i]); else p.appendChild(n); }

/* ---- caret positions ---- */
function posOf(r, h, end){
  var n = end ? r.endContainer : r.startContainer;
  var o = end ? r.endOffset : r.startOffset;
  var p, i;
  if(n == null) return null;
  if(n.nodeType === 3){ i = nidx(n); if(i < 0) return null; p = n.parentNode; o = Math.max(0, Math.min(o, tlen(n))); }
  else if(n.nodeType === 1){ p = n; i = Math.max(0, Math.min(o, n.childNodes.length)); o = -1; }
  else return null;
  if(p == null || !anc(h, p)) return null;
  return { p: p, i: i, o: o };
}
function setPos(r, p, i, o){
  var c = p.childNodes;
  if(i < 0) i = 0;
  if(i > c.length) i = c.length;
  if(o >= 0 && i < c.length && isTxt(c[i])) setc(r, c[i], Math.min(o, tlen(c[i])));
  else setc(r, p, i);
}
/* The live node/offset a position resolves to against current childNodes. */
function posNode(q){
  var c = q.p.childNodes;
  if(q.o >= 0 && q.i < c.length && isTxt(c[q.i])) return { n: c[q.i], o: Math.min(q.o, tlen(c[q.i])) };
  return { n: q.p, o: q.i };
}

function Range(){ this.startContainer = doc; this.startOffset = 0; this.endContainer = doc; this.endOffset = 0; this.collapsed = true; this.commonAncestorContainer = doc; }
function upd(r){
  var a = r.startContainer, g = 0;
  r.collapsed = (r.startContainer === r.endContainer && r.startOffset === r.endOffset);
  while(a != null && g++ < 512 && !anc(a, r.endContainer)) a = a.parentNode;
  r.commonAncestorContainer = (a != null) ? a : r.startContainer;
  return r;
}
function setb(r, sc, so, ec, eo){ r.startContainer = sc; r.startOffset = so; r.endContainer = ec; r.endOffset = eo; return upd(r); }
function setc(r, n, o){ return setb(r, n, o, n, o); }

/* Remove (frag == null), move into frag, or - with keep - copy into frag the
 * contents of r. Boundary text groups are rewritten whole; fully contained
 * nodes go as one unit (a text group copies/removes as its full value). */
function cut(r, frag, keep){
  if(r.collapsed) return;
  var h = top(r.startContainer);
  var a = posOf(r, h, false), b = posOf(r, h, true);
  if(a == null || b == null) return;
  var p = a.p, c, i, v, x, mid = [];
  function copyTxt(s){ if(frag && s !== "") frag.appendChild(doc.createTextNode(s)); }
  function takeNode(x){
    if(isTxt(x)){
      if(frag) frag.appendChild(doc.createTextNode(x.nodeValue || ""));
      else if(!keep) rmGroup(x.parentNode, nidx(x));
    } else if(keep){ if(frag) frag.appendChild(x.cloneNode(true)); }
    else if(frag) frag.appendChild(x);
    else if(x.parentNode != null) x.parentNode.removeChild(x);
  }
  if(a.o >= 0 && b.o >= 0 && a.i === b.i && a.p === b.p){
    v = p.childNodes[a.i].nodeValue || "";
    copyTxt(v.substring(a.o, b.o));
    if(!keep){ replGroup(p, a.i, v.substring(0, a.o) + v.substring(b.o)); setPos(r, p, a.i, a.o); }
    return;
  }
  if(a.p === b.p){
    c = p.childNodes;
    var mA = (a.o >= 0) ? a.i + 1 : a.i, mB = b.i - 1;
    for(i = mA; i <= mB && i < c.length; i++) mid.push(c[i]);
    if(keep){
      if(a.o >= 0) copyTxt((c[a.i].nodeValue || "").substring(a.o));
      for(i = 0; i < mid.length; i++) takeNode(mid[i]);
      if(b.o >= 0) copyTxt((c[b.i].nodeValue || "").substring(0, b.o));
      return;
    }
    /* right-to-left so the left indices stay valid */
    if(b.o >= 0){ v = c[b.i].nodeValue || ""; replGroup(p, b.i, v.substring(b.o)); }
    for(i = mid.length - 1; i >= 0; i--){ x = mid[i]; if(isTxt(x)) rmGroup(p, nidx(x)); else p.removeChild(x); }
    if(a.o >= 0){ v = p.childNodes[a.i].nodeValue || ""; replGroup(p, a.i, v.substring(0, a.o)); }
    setPos(r, p, a.i + (a.o > 0 ? 1 : 0), -1);
    return;
  }
  /* cross-parent: truncate the boundary groups, take what lies between */
  var sc = r.startContainer, so = r.startOffset, ec = r.endContainer, eo = r.endOffset;
  var kids = [], g = 0, ref, nsc, nso;
  function walk(n){
    var ch = n.childNodes, k, xx;
    for(k = 0; k < ch.length; k++){
      xx = ch[k];
      if(cmp(n, k, sc, so) >= 0 && cmp(n, k + 1, ec, eo) <= 0) kids.push(xx);
      else if(xx.nodeType === 1 && (anc(xx, sc) || anc(xx, ec))) walk(xx);
    }
  }
  if(anc(sc, ec)){ nsc = sc; nso = so; }
  else {
    ref = sc;
    while(ref.parentNode != null && !anc(ref.parentNode, ec) && g++ < 512) ref = ref.parentNode;
    nsc = ref.parentNode; nso = nidx(ref) + 1;
  }
  walk(r.commonAncestorContainer);
  if(keep){
    if(isCD(sc)) copyTxt((sc.nodeValue || "").substring(so));
    for(i = 0; i < kids.length; i++) takeNode(kids[i]);
    if(isCD(ec)) copyTxt((ec.nodeValue || "").substring(0, eo));
    return;
  }
  if(isCD(ec)){ v = ec.nodeValue || ""; replGroup(ec.parentNode, nidx(ec), v.substring(eo)); }
  for(i = kids.length - 1; i >= 0; i--) takeNode(kids[i]);
  if(isCD(sc)){ v = sc.nodeValue || ""; replGroup(sc.parentNode, nidx(sc), v.substring(0, so)); }
  if(nsc != null) setPos(r, nsc, nso, -1);
}

var RP = Range.prototype;
Range.START_TO_START = RP.START_TO_START = 0;
Range.START_TO_END = RP.START_TO_END = 1;
Range.END_TO_END = RP.END_TO_END = 2;
Range.END_TO_START = RP.END_TO_START = 3;
RP.setStart = function(n, o){
  o = o | 0;
  if(top(n) !== top(this.endContainer) || cmp(n, o, this.endContainer, this.endOffset) > 0) setc(this, n, o);
  else setb(this, n, o, this.endContainer, this.endOffset);
};
RP.setEnd = function(n, o){
  o = o | 0;
  if(top(n) !== top(this.startContainer) || cmp(n, o, this.startContainer, this.startOffset) < 0) setc(this, n, o);
  else setb(this, this.startContainer, this.startOffset, n, o);
};
RP.setStartBefore = function(n){ this.setStart(n.parentNode, nidx(n)); };
RP.setStartAfter = function(n){ this.setStart(n.parentNode, nidx(n) + 1); };
RP.setEndBefore = function(n){ this.setEnd(n.parentNode, nidx(n)); };
RP.setEndAfter = function(n){ this.setEnd(n.parentNode, nidx(n) + 1); };
RP.selectNode = function(n){ var p = n.parentNode, i = nidx(n); setb(this, p, i, p, i + 1); };
RP.selectNodeContents = function(n){ setb(this, n, 0, n, nlen(n)); };
RP.collapse = function(toStart){
  if(toStart) setc(this, this.startContainer, this.startOffset);
  else setc(this, this.endContainer, this.endOffset);
};
RP.cloneRange = function(){ return setb(new Range(), this.startContainer, this.startOffset, this.endContainer, this.endOffset); };
RP.detach = function(){};
RP.compareBoundaryPoints = function(how, s){
  var mine = (how === 0 || how === 3), theirs = (how === 0 || how === 1);
  return cmp(mine ? this.startContainer : this.endContainer, mine ? this.startOffset : this.endOffset,
             theirs ? s.startContainer : s.endContainer, theirs ? s.startOffset : s.endOffset);
};
RP.comparePoint = function(n, o){
  if(cmp(n, o, this.startContainer, this.startOffset) < 0) return -1;
  return cmp(n, o, this.endContainer, this.endOffset) > 0 ? 1 : 0;
};
RP.isPointInRange = function(n, o){ return this.comparePoint(n, o) === 0; };
RP.intersectsNode = function(n){
  var p = n.parentNode, i;
  if(p == null) return true;
  i = nidx(n);
  return cmp(p, i, this.endContainer, this.endOffset) < 0 && cmp(p, i + 1, this.startContainer, this.startOffset) > 0;
};
RP.deleteContents = function(){ cut(this, null, false); };
RP.extractContents = function(){ var f = doc.createDocumentFragment(); cut(this, f, false); return f; };
RP.cloneContents = function(){ var f = doc.createDocumentFragment(); cut(this, f, true); return f; };
RP.insertNode = function(node){
  var list = [], i, c, q, p, idx, o, v, suf, merged, lastTxt;
  if(node.nodeType === 11){ c = node.childNodes; for(i = 0; i < c.length; i++) list.push(c[i]); }
  else list.push(node);
  if(list.length === 0) return;
  q = posOf(this, top(this.startContainer), false);
  if(q == null) return;
  p = q.p; idx = q.i; o = q.o;
  var startI, startO, endI;
  if(o >= 0){
    v = p.childNodes[idx].nodeValue || "";
    rmGroup(p, idx);
    if(o > 0){ insAt(p, idx, doc.createTextNode(v.substring(0, o))); startI = idx; startO = o; idx++; }
    else { startI = idx; startO = -1; }
    for(i = 0; i < list.length; i++){ insAt(p, idx, list[i]); idx++; }
    lastTxt = isTxt(list[list.length - 1]);
    if(o < v.length){
      suf = doc.createTextNode(v.substring(o));
      insAt(p, idx, suf);
      if(lastTxt){
        /* inserted text coalesced with the suffix: end sits inside the merge */
        merged = p.childNodes[idx - 1];
        if(startO >= 0) setb(this, p.childNodes[startI], startO, merged, tlen(merged));
        else setb(this, p, startI, merged, tlen(merged));
        return;
      }
      idx++;
    }
    endI = idx;
  } else {
    startI = idx; startO = -1;
    for(i = 0; i < list.length; i++){ insAt(p, idx, list[i]); idx++; }
    endI = idx;
  }
  if(startO >= 0) setb(this, p.childNodes[startI], startO, p, endI);
  else setb(this, p, startI, p, endI);
};
RP.surroundContents = function(n){ var f = this.extractContents(); this.insertNode(n); n.appendChild(f); this.selectNode(n); };
RP.toString = function(){
  var sc = this.startContainer, so = this.startOffset, ec = this.endContainer, eo = this.endOffset, out = "";
  function walk(n){
    var ch = n.childNodes, k, x, v, a, b;
    for(k = 0; k < ch.length; k++){
      x = ch[k];
      if(x.nodeType === 1){ walk(x); continue; }
      if(x.nodeType !== 3) continue;
      v = x.nodeValue || ""; a = 0; b = v.length;
      if(x === sc) a = so; else if(cmp(x, 0, sc, so) < 0) continue;
      if(x === ec) b = eo; else if(cmp(x, v.length, ec, eo) > 0) continue;
      out += v.substring(a, b);
    }
  }
  if(this.collapsed) return "";
  if(sc === ec && sc.nodeType === 3) return (sc.nodeValue || "").substring(so, eo);
  walk(this.commonAncestorContainer);
  return out;
};
RP.createContextualFragment = function(html){
  var d = doc.createElement("div"), f = doc.createDocumentFragment(), g = 0;
  d.innerHTML = html;
  while(d.firstChild != null && g++ < 4096) f.appendChild(d.firstChild);
  return f;
};
RP.getBoundingClientRect = function(){
  var e = this.startContainer;
  if(e != null && e.nodeType !== 1) e = e.parentNode;
  return (e != null && e.getBoundingClientRect) ? e.getBoundingClientRect() : { left: 0, top: 0, right: 0, bottom: 0, width: 0, height: 0, x: 0, y: 0 };
};
RP.getClientRects = function(){ return [this.getBoundingClientRect()]; };

var sel = { _r: null };
function def(o, k, g){ Object.defineProperty(o, k, { get: g, enumerable: true, configurable: true }); }
def(sel, "rangeCount", function(){ return sel._r != null ? 1 : 0; });
def(sel, "anchorNode", function(){ return sel._r != null ? sel._r.startContainer : null; });
def(sel, "anchorOffset", function(){ return sel._r != null ? sel._r.startOffset : 0; });
def(sel, "focusNode", function(){ return sel._r != null ? sel._r.endContainer : null; });
def(sel, "focusOffset", function(){ return sel._r != null ? sel._r.endOffset : 0; });
def(sel, "isCollapsed", function(){ return sel._r != null ? sel._r.collapsed : true; });
def(sel, "type", function(){ return sel._r == null ? "None" : (sel._r.collapsed ? "Caret" : "Range"); });
sel.getRangeAt = function(i){
  if(sel._r == null || (i | 0) !== 0){
    var m = "Failed to execute 'getRangeAt' on 'Selection': " + i + " is not a valid index.";
    throw (typeof DOMException === "function") ? new DOMException(m, "IndexSizeError") : new Error(m);
  }
  return sel._r;
};
sel.removeAllRanges = function(){ sel._r = null; };
sel.empty = sel.removeAllRanges;
sel.addRange = function(r){ if(r != null && sel._r == null) sel._r = r; };
sel.removeRange = function(r){ if(sel._r === r) sel._r = null; };
sel.collapse = function(n, o){ if(n == null) sel._r = null; else sel._r = setc(new Range(), n, o | 0); };
sel.setPosition = sel.collapse;
sel.collapseToStart = function(){ if(sel._r != null) sel._r.collapse(true); };
sel.collapseToEnd = function(){ if(sel._r != null) sel._r.collapse(false); };
sel.extend = function(n, o){ if(sel._r != null) sel._r.setEnd(n, o | 0); };
sel.selectAllChildren = function(n){ var r = new Range(); r.selectNodeContents(n); sel._r = r; };
sel.setBaseAndExtent = function(an, ao, fn, fo){
  ao = ao | 0; fo = fo | 0;
  sel._r = (cmp(an, ao, fn, fo) <= 0) ? setb(new Range(), an, ao, fn, fo) : setb(new Range(), fn, fo, an, ao);
};
sel.containsNode = function(n){ return sel._r != null && !sel._r.collapsed && sel._r.intersectsNode(n); };
sel.deleteFromDocument = function(){ if(sel._r != null) sel._r.deleteContents(); };
sel.toString = function(){ return sel._r != null ? sel._r.toString() : ""; };
sel.modify = function(){};
function getSel(){ return sel; }

/* ---- contenteditable editing commands (position-based, group-aware) ---- */
function hostOf(n){
  var g = 0, a;
  for(; n != null && g < 512; n = n.parentNode, g++){
    if(n.nodeType !== 1 || !n.getAttribute) continue;
    a = n.getAttribute("contenteditable");
    if(a != null) return (("" + a).toLowerCase() !== "false") ? n : null;
  }
  return null;
}
function isBR(n){ return n != null && n.nodeType === 1 && /^br$/i.test(n.tagName); }
function isLeaf(n){ return n.nodeType !== 1 || n.childNodes.length === 0 || /^(br|img|input|hr)$/i.test(n.tagName); }
/* The live caret for host h as a position: the current selection when it sits
 * inside h (re-resolved against live childNodes), else a fresh caret at the end
 * of h's content (before a trailing <br> placeholder, so typing lands on the
 * last line rather than after it). */
function caretPos(h){
  var r = sel._r, q = (r != null) ? posOf(r, h, false) : null;
  if(q == null){
    var c = h.childNodes, i = c.length;
    if(i > 0 && isBR(c[i - 1])) i--;
    q = { p: h, i: i, o: -1 };
    r = new Range(); sel._r = r;
    setPos(r, q.p, q.i, q.o);
    return q;
  }
  /* Re-resolve against live childNodes WITHOUT collapsing a live selection:
   * typing / backspace over a selection must still see it non-collapsed. */
  var e = r.collapsed ? null : posOf(r, h, true);
  if(e == null){ setPos(r, q.p, q.i, q.o); return q; }
  var a = posNode(q), b = posNode(e);
  setb(r, a.n, a.o, b.n, b.o);
  return q;
}
function putAt(q, text){
  var p = q.p, i = q.i, o = q.o, c = p.childNodes, v;
  if(o >= 0){
    v = c[i].nodeValue || "";
    replGroup(p, i, v.substring(0, o) + text + v.substring(o));
    setPos(sel._r, p, i, o + text.length);
    return;
  }
  if(i > 0 && isTxt(c[i - 1])){ putAt({ p: p, i: i - 1, o: tlen(c[i - 1]) }, text); return; }
  if(i < c.length && isTxt(c[i])){ putAt({ p: p, i: i, o: 0 }, text); return; }
  insAt(p, i, doc.createTextNode(text));
  setPos(sel._r, p, i, text.length);
}
/* Code units one caret step covers at offset o of v (2 across a surrogate pair). */
function unit(v, o, dir){
  var c;
  if(dir < 0){ c = v.charCodeAt(o - 1); return (o >= 2 && c >= 0xDC00 && c <= 0xDFFF) ? 2 : 1; }
  c = v.charCodeAt(o);
  return (o + 1 < v.length && c >= 0xD800 && c <= 0xDBFF) ? 2 : 1;
}
/* Move the collapsed caret one position backwards (dir -1) or forwards within
 * h, deleting what it crosses when del is set: one character of text, or one
 * leaf element (<br>, <img>). Returns true when it moved / deleted. */
function stepPos(h, q, dir, del){
  var g = 0, p, i, o, c, x, v, k, ni, pp;
  while(g++ < 256){
    p = q.p; i = q.i; o = q.o; c = p.childNodes;
    if(o >= 0){
      v = c[i].nodeValue || "";
      if(dir < 0 ? o > 0 : o < v.length){
        k = unit(v, o, dir);
        if(dir < 0) o -= k;
        if(del){
          v = v.substring(0, o) + v.substring(o + k);
          replGroup(p, i, v);
          setPos(sel._r, p, i, v === "" ? -1 : o);
        } else {
          if(dir > 0) o += k;
          setPos(sel._r, p, i, o);
        }
        return true;
      }
      q = { p: p, i: (dir < 0 ? i : i + 1), o: -1 };
      continue;
    }
    ni = (dir < 0) ? i - 1 : i;
    if(ni < 0 || ni >= c.length){
      if(p === h){ setPos(sel._r, p, i, -1); return false; }
      pp = p.parentNode;
      if(pp == null){ setPos(sel._r, p, i, -1); return false; }
      q = { p: pp, i: nidx(p) + (dir > 0 ? 1 : 0), o: -1 };
      continue;
    }
    x = c[ni];
    if(isTxt(x)){
      if(tlen(x) === 0){ rmGroup(p, ni); q = { p: p, i: ni, o: -1 }; continue; }
      q = { p: p, i: ni, o: (dir < 0 ? tlen(x) : 0) };
      continue;
    }
    if(x.nodeType === 1 && !isLeaf(x)){ q = { p: x, i: (dir < 0 ? x.childNodes.length : 0), o: -1 }; continue; }
    if(x.nodeType !== 1){ q = { p: p, i: (dir < 0 ? ni : ni + 1), o: -1 }; continue; }
    if(del){ p.removeChild(x); setPos(sel._r, p, ni, -1); return true; }
    setPos(sel._r, p, (dir < 0 ? ni : ni + 1), -1);
    return true;
  }
  return false;
}
function enterAt(q){
  var p = q.p, i = q.i, o = q.o, c = p.childNodes, v, br, tail;
  if(o > 0 && isTxt(c[i]) && o < tlen(c[i])){
    v = c[i].nodeValue || "";
    rmGroup(p, i);
    insAt(p, i, doc.createTextNode(v.substring(0, o))); i++;
    br = doc.createElement("br"); insAt(p, i, br); i++;
    tail = v.substring(o);
    if(tail !== "") insAt(p, i, doc.createTextNode(tail));
    else if(br.nextSibling == null) p.appendChild(doc.createElement("br"));
    setPos(sel._r, p, i, -1);
    return true;
  }
  if(o >= 0) i = (o === 0) ? i : i + 1;
  br = doc.createElement("br"); insAt(p, i, br);
  /* A line break at the very end needs a second <br> to open the line. */
  if(br.nextSibling == null) p.appendChild(doc.createElement("br"));
  setPos(sel._r, p, i + 1, -1);
  return true;
}
function hook(h, op, text){
  try {
    if(h == null || h.nodeType !== 1) return false;
    var q = caretPos(h), r = sel._r;
    if(op === "caretrect"){
      /* Engine paint probe: geometry of the live insertion point. Only a
       * collapsed selection paints a caret bar; a range reports null. The
       * rect comes from the embedder's layout (bridge __ewebCaretRect). */
      if(!r.collapsed) return null;
      var n = r.startContainer, o = r.startOffset, a = n, inside = false;
      while(a != null){ if(a === h){ inside = true; break; } a = a.parentNode; }
      if(!inside || n == null) return null;
      var cr = n.__ewebCaretRect ? n.__ewebCaretRect(o) : null;
      if(cr == null) return null;
      return JSON.stringify({x:cr.x, y:cr.y, h:cr.height});
    }
    if(op === "focus") return false;
    if(op === "text"){
      if(text == null || text.length === 0) return false;
      if(!r.collapsed){ cut(r, null, false); q = posOf(r, h, false); if(q == null) q = caretPos(h); }
      putAt(q, text);
      return true;
    }
    if(op === "back" || op === "del"){
      if(!r.collapsed){ cut(r, null, false); return true; }
      return stepPos(h, q, op === "back" ? -1 : 1, true);
    }
    if(op === "left" || op === "right"){
      if(!r.collapsed) r.collapse(op === "left");
      else stepPos(h, q, op === "left" ? -1 : 1, false);
      return false;
    }
    if(op === "home"){ setPos(r, h, 0, -1); return false; }
    if(op === "end"){
      var c = h.childNodes, i = c.length;
      if(i > 0 && isBR(c[i - 1])) i--;
      setPos(r, h, i, -1);
      return false;
    }
    if(op === "selall"){ r.selectNodeContents(h); return false; }
    if(op === "enter"){
      if(!r.collapsed){ cut(r, null, false); q = posOf(r, h, false); if(q == null) q = caretPos(h); }
      return enterAt(q);
    }
  } catch(e){ /* editing must never throw into the engine's key path */ }
  return false;
}

window.getSelection = getSel;
document.getSelection = getSel;
document.createRange = function(){ return new Range(); };
window.Range = Range;
try {
  var EP = Element.prototype;
  Object.defineProperty(EP, "isContentEditable", { get: function(){ return hostOf(this) != null; }, configurable: true });
  Object.defineProperty(EP, "contentEditable", {
    get: function(){ var a = this.getAttribute("contenteditable"); return a == null ? "inherit" : (("" + a).toLowerCase() === "false" ? "false" : (a === "plaintext-only" ? a : "true")); },
    set: function(v){ if(v === "inherit") this.removeAttribute("contenteditable"); else this.setAttribute("contenteditable", "" + v); },
    configurable: true
  });
} catch(e){}
hook.gs = getSel;
hook.Range = Range;
return hook;
})();
var getSelection = __eweb_ce.gs;
Range = __eweb_ce.Range;
)EWEBJS";

#endif /* EWEB_EDITING_JS_H */