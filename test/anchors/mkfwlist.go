//go:build ignore

// mkfwlist regenerates firmwares.tsv from KoboStuff's kfw.db.js (the canonical Kobo
// firmware database), one entry per 4.x version number (highest device channel).
//
//	go run mkfwlist.go > firmwares.tsv
//
// -db overrides the source (URL or local path). It defaults to KoboStuff's gh-pages;
// until PR pgaskin/KoboStuff#69 merges, the newest builds live on its head:
//
//	go run mkfwlist.go -db https://raw.githubusercontent.com/pgaskin/KoboStuff/refs/pull/69/head/kfw.db.js > firmwares.tsv
package main

import (
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

const cdn = "https://ereaderfiles.kobo.com/firmwares/kobo"

func main() {
	db := flag.String("db", "https://raw.githubusercontent.com/pgaskin/KoboStuff/gh-pages/kfw.db.js", "kfw.db.js source URL or path")
	flag.Parse()

	var data []byte
	var err error
	if strings.Contains(*db, "://") {
		resp, e := http.Get(*db)
		if e != nil {
			fatal(e)
		}
		defer resp.Body.Close()
		if resp.StatusCode != 200 {
			fatal(fmt.Errorf("%s: status %d", *db, resp.StatusCode))
		}
		data, err = io.ReadAll(resp.Body)
	} else {
		data, err = os.ReadFile(*db)
	}
	if err != nil {
		fatal(err)
	}

	// entry: [dh+"CH", dd+"id", "VERSION", "DATE", (dk|da|dn)+"PATH", "MD5"]
	re := regexp.MustCompile(`\[dh\+"(\d+)",dd\+"[^"]*","([^"]+)","[^"]*",(?:dk|da|dn)\+"([^"]+)","([0-9a-fA-F]{32})"\]`)
	type ent struct {
		ver, url, md5 string
		ch            int
	}
	best := map[string]ent{}
	for _, m := range re.FindAllStringSubmatch(string(data), -1) {
		ver := m[2]
		if !strings.HasPrefix(ver, "4.") {
			continue
		}
		ch, _ := strconv.Atoi(m[1])
		if cur, ok := best[ver]; !ok || ch > cur.ch {
			best[ver] = ent{ver, cdn + m[3], strings.ToLower(m[4]), ch}
		}
	}
	out := make([]ent, 0, len(best))
	for _, v := range best {
		out = append(out, v)
	}
	sort.Slice(out, func(i, j int) bool {
		if c := cmpVer(vkey(out[i].ver), vkey(out[j].ver)); c != 0 {
			return c < 0
		}
		return out[i].ver < out[j].ver // stable tie-break (e.g. 4.13.12638 vs -s variant)
	})

	fmt.Println("# Kobo 4.x firmware, one entry per version number (highest device channel), from KoboStuff kfw.db.js.")
	fmt.Println("# Columns: version <tab> url <tab> zip-md5. Regenerate with: go run mkfwlist.go > firmwares.tsv")
	for _, v := range out {
		fmt.Printf("%s\t%s\t%s\n", v.ver, v.url, v.md5)
	}
}

func fatal(err error) {
	fmt.Fprintln(os.Stderr, "mkfwlist:", err)
	os.Exit(1)
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
