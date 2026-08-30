r_flow <- function(x, y) {
  if (x && y) {
    for (i in seq_len(x)) {
      if (i == y) x <- x - 1
    }
  } else if (x || y) {
    x <- y
  }
  while (x > 0) x <- x - 1
  repeat {
    if (x == 0) break
  }
  switch(x, x + 1, x + 2)
}

r_outer <- function(callback) {
  callback(function(value) if (value) 1 else 0)
}
