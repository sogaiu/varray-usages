#! /usr/bin/env janet

(use ./sh-dsl)

########################################################################

# configure this if needed
(def header-path
  (string (os/getenv "HOME") "/.local/include"))

########################################################################

(print `* building if needed...`)

(os/mkdir "build")

(def build-exit
     ($ cc -fPIC
           -I ,header-path
           -shared
           src/varray.c
           -o build/varray.so))
(assertf (zero? build-exit)
         "build failed: %d" build-exit)

(print "done")

########################################################################

(print "* running niche...")

(def niche-exit ($ janet ./bin/niche.janet))
(assertf (zero? niche-exit)
         "niche exited: %d" niche-exit)

(print "done")
