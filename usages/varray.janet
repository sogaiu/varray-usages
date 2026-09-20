(import ../build/varray)

(comment

  (def names ["Cherry" "Lime" "Durian" "Bananas" "Apple"])

  names
  # =>
  ["Cherry" "Lime" "Durian" "Bananas" "Apple"]

  (type names)
  # =>
  :tuple

  )

(comment

  (def numbers [22 5 97 12 1])

  numbers
  # =>
  [22 5 97 12 1]

  (type (varray/from :uint32 numbers))
  # =>
  :va/view

  (varray/slice (varray/from :uint32 numbers))
  # =>
  @[22 5 97 12 1]

  (type (varray/range :int32 10))
  # =>
  :va/view

  (varray/slice (varray/range :int32 10))
  # =>
  @[0 1 2 3 4 5 6 7 8 9]

  )

(comment

  (def sales (varray/from :uint32 [22 5 97 12 1]))

  (varray/slice sales)
  # =>
  @[22 5 97 12 1]

  (def order (varray/grade sales))

  (varray/slice order)
  # =>
  @[4 1 3 0 2]

  )

(comment

  (varray/slice (varray/le 5 sales))
  # =>
  @[1 1 1 1 0]

  (varray/slice (varray/lt sales 50))
  # =>
  @[1 1 0 1 1]

  (def mid (varray/minimum (varray/le 5 sales)
                           (varray/lt sales 50)))
  (varray/slice mid)
  # =>
  @[1 1 0 1 0]

  )

(comment

  (varray/slice (varray/gather sales order))
  # =>
  @[1 5 12 22 97]

  (map |(in names $) (varray/slice order))
  # =>
  @["Apple" "Lime" "Bananas" "Cherry" "Durian"]

  )

(comment

  (varray/sum mid)
  # =>
  3

  (varray/slice (varray/compress sales mid))
  # =>
  @[22 5 12]

  (varray/slice (varray/compress sales (varray/sub 1 mid)))
  # =>
  @[97 1]

  (map |(in names $) (varray/slice (varray/where mid)))
  # =>
  @["Cherry" "Lime" "Bananas"]

  )

