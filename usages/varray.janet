(import ../build/varray)

(comment

  (def va (varray/new :int32 8))

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

  (def va-right (varray/new :int32 3 5 rs))

  (varray/slice va-right)
  # =>
  @[6 7 8]

  )

