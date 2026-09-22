(import ../build/varray)

(comment

  (def va (varray/new :uint32 8))

  (varray/slice va)
  # =>
  @[0 0 0 0 0 0 0 0]

  (varray/length va)
  # =>
  8

  (def va-one (varray/add va 1))

  (varray/slice va-one)
  # =>
  @[1 1 1 1 1 1 1 1]

  (def va-two (varray/add va-one va-one))

  (varray/slice va-two)
  # =>
  @[2 2 2 2 2 2 2 2]

  (varray/sum va-two)
  # =>
  16

  (def rs (varray/running-sum va-one))

  (varray/slice rs)
  # =>
  @[1 2 3 4 5 6 7 8]

  (def sr (varray/reverse rs))

  (varray/slice sr)
  # =>
  @[8 7 6 5 4 3 2 1]

  (def srgr (varray/grade sr))

  (varray/slice srgr)
  # =>
  @[7 6 5 4 3 2 1 0]

  (varray/slice (varray/eq va-one
                           (varray/sub sr srgr)))
  # =>
  @[1 1 1 1 1 1 1 1]

  (def srge (varray/gather sr [0 2 4 6]))

  (varray/slice srge)
  # =>
  @[8 6 4 2]

  (def srgo (varray/gather sr [1 3 5 7]))

  (varray/slice srgo)
  # =>
  @[7 5 3 1]

  (def boxen (varray/new :uint32 8))

  (varray/slice boxen)
  # =>
  @[0 0 0 0 0 0 0 0]

  (varray/copy-bytes srgo 0 boxen 0 4)

  (varray/slice boxen)
  # =>
  @[7 5 3 1 0 0 0 0]

  (varray/copy-bytes srge 0 boxen 4 4)

  (varray/slice boxen)
  # =>
  @[7 5 3 1 8 6 4 2]

  )

(comment

  (def va-tail (varray/new :uint32 3 5 rs))

  (varray/slice va-tail)
  # =>
  @[6 7 8]

  (def vag (varray/gather rs [5 6 7]))

  (varray/slice vag)
  # =>
  @[6 7 8]

  )

