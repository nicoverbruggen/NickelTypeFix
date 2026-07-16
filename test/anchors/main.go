// Command anchors validates NickelTypeFix's in-memory byte-patch anchors against
// real Kobo firmware, the way test/syms validates //libnickel symbols.
//
// For each byte-patch fix it confirms, in the firmware's own libQtGui/libQtWebKit,
// that every anchor is PRESENT and UNIQUE and that the expected original bytes sit
// at each edit offset, and it prints the firmware range the fix applies to.
//
// A firmware where the target library simply doesn't carry the pattern is reported
// "sits out" and does NOT fail (that is the mod's fail-safe behaviour). A firmware
// where the pattern is AMBIGUOUS (>1 match) or the ORIGINAL BYTES DIFFER is a hard
// failure: the mod would either refuse to patch or, worse, patch the wrong bytes.
//
// The anchors/original-bytes are parsed straight from ../../src/nickeltypefix.cc so
// there is a single source of truth. The per-fix layout (which anchor, which lib,
// the edit offsets) is the small table below.
//
// Firmware libs come from pgaskin's public mirror (the same source kobopatch-testdata
// uses); only the KoboRoot.tgz section is fetched, via HTTP range requests, and the
// two Qt libs are cached under -corpus so re-runs (and CI cache) skip the download.
package main

import (
	"archive/tar"
	"archive/zip"
	"bytes"
	"compress/flate"
	"compress/gzip"
	"errors"
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
)

// Qt libs we extract from each firmware and scan.
var libFiles = map[string]string{ // key -> path inside KoboRoot.tgz
	"Gui":    "usr/local/Trolltech/QtEmbedded-4.6.2-arm/lib/libQtGui.so.4.6.2",
	"WebKit": "usr/local/Trolltech/QtEmbedded-4.6.2-arm/lib/libQtWebKit.so.4.6.2",
}

type editDef struct {
	off  int    // edit offset from the anchor start; -1 means "= len(anchor)" (edit right after it)
	orig string // name of the ORIG byte array the site expects at off
}
type siteDef struct {
	lib    string // which lib: "Gui" or "WebKit"
	anchor string // name of the ANCHOR byte array
	edits  []editDef
}
type fixDef struct {
	name, cfg string
	sites     []siteDef
}

// Mirrors NTF_JUSTIFY_FIXES in src/nickeltypefix.cc (offsets + which lib/anchor).
// The actual bytes come from the source, parsed below.
var fixes = []fixDef{
	{"koboSpan justification", "ntf_justify_kospan", []siteDef{
		{"Gui", "KOS_A_ANCHOR", []editDef{{6, "KOS_A_ORIG"}}},
		{"Gui", "KOS_B_ANCHOR", []editDef{{4, "KOS_B_ORIG"}}},
	}},
	{"punctuation justification", "ntf_justify_punct", []siteDef{
		{"WebKit", "PUN_ANCHOR", []editDef{{-1, "PUN_ORIG"}}}, // edit at anchor+sizeof(anchor)
	}},
	{"letter-spacing on spaces", "ntf_letterspace_spaces", []siteDef{
		{"Gui", "LSP_ANCHOR", []editDef{{6, "LSP_A_ORIG"}, {12, "LSP_B_ORIG"}}},
	}},
}

var arrRe = regexp.MustCompile(`(?s)static const unsigned char (\w+)\s*\[\]\s*=\s*\{(.*?)\}`)
var hexRe = regexp.MustCompile(`0[xX][0-9a-fA-F]{1,2}`)

func parseArrays(src string) (map[string][]byte, error) {
	b, err := os.ReadFile(src)
	if err != nil {
		return nil, err
	}
	out := map[string][]byte{}
	for _, m := range arrRe.FindAllSubmatch(b, -1) {
		name := string(m[1])
		var by []byte
		for _, tok := range hexRe.FindAll(m[2], -1) {
			v, err := strconv.ParseUint(strings.TrimPrefix(strings.TrimPrefix(string(tok), "0x"), "0X"), 16, 8)
			if err != nil {
				return nil, fmt.Errorf("%s: bad byte %q: %w", name, tok, err)
			}
			by = append(by, byte(v))
		}
		if len(by) > 0 {
			out[name] = by
		}
	}
	return out, nil
}

func countMatches(hay, needle []byte) (n, first int) {
	first = -1
	for off := 0; ; {
		i := bytes.Index(hay[off:], needle)
		if i < 0 {
			break
		}
		p := off + i
		if first < 0 {
			first = p
		}
		n++
		off = p + 1
	}
	return
}

type result int

const (
	rOK        result = iota // present, unique, bytes match -> fix applies
	rSitsOut                 // pattern absent -> fix safely sits out
	rNoLib                   // firmware has no such lib
	rAmbiguous               // >1 match -> HARD FAIL
	rBytesDiff               // orig bytes differ at an edit -> HARD FAIL
)

func (r result) fail() bool { return r == rAmbiguous || r == rBytesDiff }
func (r result) String() string {
	switch r {
	case rOK:
		return "applies"
	case rSitsOut:
		return "sits-out"
	case rNoLib:
		return "no-lib"
	case rAmbiguous:
		return "AMBIGUOUS"
	case rBytesDiff:
		return "BYTES-DIFFER"
	}
	return "?"
}

// checkSite validates one anchor site against one lib's bytes.
func checkSite(arrays map[string][]byte, s siteDef, lib []byte) (result, string) {
	if lib == nil {
		return rNoLib, ""
	}
	anchor := arrays[s.anchor]
	if anchor == nil {
		return rBytesDiff, "anchor array " + s.anchor + " not found in source"
	}
	n, at := countMatches(lib, anchor)
	if n == 0 {
		return rSitsOut, ""
	}
	if n > 1 {
		return rAmbiguous, fmt.Sprintf("%s matched %d times", s.anchor, n)
	}
	for _, e := range s.edits {
		orig := arrays[e.orig]
		off := e.off
		if off < 0 {
			off = len(anchor)
		}
		p := at + off
		if p+len(orig) > len(lib) || !bytes.Equal(lib[p:p+len(orig)], orig) {
			return rBytesDiff, fmt.Sprintf("%s@+%d: expected %x", s.anchor, off, orig)
		}
	}
	return rOK, ""
}

func main() {
	src := flag.String("src", "", "path to nickeltypefix.cc (default: locate ../../src/nickeltypefix.cc)")
	corpus := flag.String("corpus", "corpus", "dir to cache extracted firmware libs")
	list := flag.String("list", "firmwares.tsv", "firmware list (version<tab>url<tab>md5)")
	floor := flag.String("floor", "4.21", "minimum firmware version to check (the mod's support floor)")
	only := flag.String("only", "", "comma-separated versions to check (default: all in -list at/above -floor)")
	offline := flag.Bool("offline", false, "use only cached libs under -corpus; never download")
	flag.Parse()

	srcPath := *src
	if srcPath == "" {
		for _, c := range []string{"../../src/nickeltypefix.cc", "src/nickeltypefix.cc"} {
			if _, err := os.Stat(c); err == nil {
				srcPath = c
				break
			}
		}
	}
	arrays, err := parseArrays(srcPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: parse anchors from %q: %v\n", srcPath, err)
		os.Exit(2)
	}
	fmt.Printf("parsed %d byte arrays from %s\n", len(arrays), srcPath)

	var versions []string
	var fwURL map[string]string
	if *only != "" {
		versions = strings.Split(*only, ",")
	}
	if *offline {
		if len(versions) == 0 {
			versions = listCorpus(*corpus)
		}
	} else {
		fwURL, err = readFwList(*list, *floor)
		if err != nil {
			fmt.Fprintf(os.Stderr, "error: firmware list %q: %v\n", *list, err)
			os.Exit(2)
		}
		if len(versions) == 0 {
			for v := range fwURL {
				versions = append(versions, v)
			}
		}
	}
	sort.Slice(versions, func(i, j int) bool { return lessVersion(versions[i], versions[j]) })
	fmt.Printf("checking %d firmware version(s)\n\n", len(versions))

	// Evaluate versions in parallel (network-bound: download + extract each firmware).
	type cellR struct {
		res  result
		note string
	}
	type rowR struct {
		cells []cellR
		err   error
	}
	rows := make([]rowR, len(versions))
	sem := make(chan struct{}, 6)
	var wg sync.WaitGroup
	for i, v := range versions {
		wg.Add(1)
		go func(i int, v string) {
			defer wg.Done()
			sem <- struct{}{}
			defer func() { <-sem }()
			libs := map[string][]byte{}
			for key, inner := range libFiles {
				b, err := getLib(v, key, inner, *corpus, fwURL)
				if err != nil {
					rows[i] = rowR{err: fmt.Errorf("%s: %w", key, err)}
					return
				}
				libs[key] = b // may be nil if firmware lacks it
			}
			var cells []cellR
			for _, f := range fixes {
				worst := rOK
				var note string
				present := false
				for _, s := range f.sites {
					r, n := checkSite(arrays, s, libs[s.lib])
					if r == rOK {
						present = true
					}
					if r == rSitsOut && present {
						r, n = rBytesDiff, s.anchor+" missing while sibling site applies"
					}
					if rank(r) > rank(worst) {
						worst, note = r, n
					}
				}
				cells = append(cells, cellR{worst, note})
			}
			rows[i] = rowR{cells: cells}
		}(i, v)
	}
	wg.Wait()

	span := map[string][2]string{}
	failed := false
	fmt.Printf("%-14s", "firmware")
	for _, f := range fixes {
		fmt.Printf("  %-26s", f.cfg)
	}
	fmt.Println()
	for i, v := range versions {
		if rows[i].err != nil {
			fmt.Fprintf(os.Stderr, "error: %s %v\n", v, rows[i].err)
			os.Exit(2)
		}
		fmt.Printf("%-14s", v)
		for fi, c := range rows[i].cells {
			fmt.Printf("  %-26s", c.res.String())
			if c.res.fail() {
				failed = true
				fmt.Fprintf(os.Stderr, "FAIL %s %s: %s\n", v, fixes[fi].cfg, c.note)
			}
			if c.res == rOK {
				s := span[fixes[fi].cfg]
				if s[0] == "" {
					s[0] = v
				}
				s[1] = v
				span[fixes[fi].cfg] = s
			}
		}
		fmt.Println()
	}

	fmt.Println("\nfirmware range each fix applies to (of those checked):")
	for _, f := range fixes {
		s := span[f.cfg]
		if s[0] == "" {
			fmt.Printf("  %-24s (no checked firmware)\n", f.cfg)
		} else {
			fmt.Printf("  %-24s %s .. %s\n", f.cfg, s[0], s[1])
		}
	}
	if failed {
		fmt.Fprintln(os.Stderr, "\nFAILED: an anchor was ambiguous or its original bytes differed on a firmware that has the target library.")
		os.Exit(1)
	}
	fmt.Println("\nOK: every anchor is present-and-unique with matching original bytes, or safely absent.")
}

func rank(r result) int {
	switch r {
	case rNoLib, rSitsOut:
		return 0
	case rOK:
		return 1
	case rBytesDiff, rAmbiguous:
		return 2
	}
	return 0
}

// ---- firmware acquisition (mirror; range requests; cache under -corpus) ----

func listCorpus(dir string) []string {
	var out []string
	ents, _ := os.ReadDir(dir)
	for _, e := range ents {
		if e.IsDir() {
			out = append(out, e.Name())
		}
	}
	return out
}

// readFwList reads firmwares.tsv (version<tab>url<tab>md5) and returns version->url
// for every entry at or above floor (the mod's support floor).
func readFwList(pathTSV, floor string) (map[string]string, error) {
	b, err := os.ReadFile(pathTSV)
	if err != nil {
		return nil, err
	}
	fl := vkey(floor)
	out := map[string]string{}
	for _, line := range strings.Split(string(b), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		f := strings.Split(line, "\t")
		if len(f) < 2 {
			continue
		}
		if cmpVer(vkey(f[0]), fl) >= 0 {
			out[f[0]] = f[1]
		}
	}
	if len(out) == 0 {
		return nil, fmt.Errorf("no firmware >= %s in %s", floor, pathTSV)
	}
	return out, nil
}

var digitsRe = regexp.MustCompile(`\d+`)

func vkey(v string) []int {
	var out []int
	for _, p := range digitsRe.FindAllString(v, -1) {
		n, _ := strconv.Atoi(p)
		out = append(out, n)
	}
	return out
}

func cmpVer(a, b []int) int {
	for i := 0; i < len(a) && i < len(b); i++ {
		if a[i] != b[i] {
			if a[i] < b[i] {
				return -1
			}
			return 1
		}
	}
	return len(a) - len(b)
}

func lessVersion(a, b string) bool {
	if c := cmpVer(vkey(a), vkey(b)); c != 0 {
		return c < 0
	}
	return a < b // stable tie-break for same-numeric versions (e.g. a -s variant)
}

// getLib returns the lib bytes for a version, from the corpus cache or by fetching.
// Returns (nil, nil) if the firmware genuinely lacks the lib (cached as a 0-byte marker).
func getLib(version, key, inner, corpus string, fwURL map[string]string) ([]byte, error) {
	cache := filepath.Join(corpus, version, filepath.Base(inner))
	if b, err := os.ReadFile(cache); err == nil {
		if len(b) == 0 {
			return nil, nil // marker: firmware lacks this lib
		}
		return b, nil
	}
	if fwURL == nil {
		return nil, fmt.Errorf("not cached and offline")
	}
	if err := fetchFirmwareLibs(version, fwURL[version], corpus); err != nil {
		return nil, err
	}
	if b, err := os.ReadFile(cache); err == nil {
		if len(b) == 0 {
			return nil, nil
		}
		return b, nil
	}
	return nil, nil
}

// fetchFirmwareLibs downloads the KoboRoot.tgz section of one firmware and writes
// the wanted libs (and 0-byte markers for any missing) into corpus/<version>/.
func fetchFirmwareLibs(version, url, corpus string) error {
	if url == "" {
		return fmt.Errorf("no url for %s", version)
	}
	dir := filepath.Join(corpus, version)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	ra, err := newHTTPReaderAt(url)
	if err != nil {
		return err
	}
	zr, err := zip.NewReader(ra, ra.size)
	if err != nil {
		return err
	}
	var tgz io.Reader
	for _, fh := range zr.File {
		if fh.Name != "KoboRoot.tgz" {
			continue
		}
		off, err := fh.DataOffset()
		if err != nil {
			return err
		}
		sec, err := ra.section(off, int64(fh.CompressedSize64)) // single streaming ranged GET
		if err != nil {
			return err
		}
		defer sec.Close()
		switch fh.Method {
		case zip.Store:
			tgz = sec
		case zip.Deflate:
			tgz = flate.NewReader(sec)
		default:
			return fmt.Errorf("unsupported zip method %d", fh.Method)
		}
		break
	}
	if tgz == nil {
		return fmt.Errorf("%s: no KoboRoot.tgz", version)
	}
	gz, err := gzip.NewReader(tgz)
	if err != nil {
		return err
	}
	want := map[string]string{} // tar path -> cache path
	for _, inner := range libFiles {
		want[inner] = filepath.Join(dir, filepath.Base(inner))
	}
	got := map[string]bool{}
	tr := tar.NewReader(gz)
	for {
		h, err := tr.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			return err
		}
		dst, ok := want[path.Clean(h.Name)]
		if !ok || !h.FileInfo().Mode().IsRegular() {
			continue
		}
		b, err := io.ReadAll(tr)
		if err != nil {
			return err
		}
		if err := os.WriteFile(dst, b, 0o644); err != nil {
			return err
		}
		got[path.Clean(h.Name)] = true
		if len(got) == len(want) {
			break // have every wanted lib; stop streaming the rest of the tgz
		}
	}
	// mark any lib the firmware lacked with a 0-byte file so we don't refetch
	for inner, dst := range want {
		if !got[inner] {
			if err := os.WriteFile(dst, nil, 0o644); err != nil {
				return err
			}
		}
	}
	return nil
}

// ---- minimal HTTP ReaderAt over range requests ----

type httpReaderAt struct {
	url  string
	size int64
}

func newHTTPReaderAt(u string) (*httpReaderAt, error) {
	resp, err := http.Head(u)
	if err != nil {
		return nil, err
	}
	resp.Body.Close()
	if resp.StatusCode != 200 {
		return nil, fmt.Errorf("%s: status %d", u, resp.StatusCode)
	}
	if resp.Header.Get("Accept-Ranges") != "bytes" {
		return nil, errors.New("server does not support range requests")
	}
	n, err := strconv.ParseInt(resp.Header.Get("Content-Length"), 10, 64)
	if err != nil {
		return nil, errors.New("missing content length")
	}
	return &httpReaderAt{url: resp.Request.URL.String(), size: n}, nil
}

// section streams n bytes from off in a single ranged GET (for the big KoboRoot.tgz).
func (r *httpReaderAt) section(off, n int64) (io.ReadCloser, error) {
	req, _ := http.NewRequest(http.MethodGet, r.url, nil)
	req.Header.Set("Range", fmt.Sprintf("bytes=%d-%d", off, off+n-1))
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return nil, err
	}
	if resp.StatusCode != http.StatusPartialContent {
		resp.Body.Close()
		return nil, fmt.Errorf("section read: status %d", resp.StatusCode)
	}
	return resp.Body, nil
}

func (r *httpReaderAt) ReadAt(p []byte, off int64) (int, error) {
	if off >= r.size {
		return 0, io.EOF
	}
	req, _ := http.NewRequest(http.MethodGet, r.url, nil)
	req.Header.Set("Range", fmt.Sprintf("bytes=%d-%d", off, off+int64(len(p))-1))
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return 0, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusPartialContent {
		return 0, fmt.Errorf("range read: status %d", resp.StatusCode)
	}
	return io.ReadFull(resp.Body, p)
}
