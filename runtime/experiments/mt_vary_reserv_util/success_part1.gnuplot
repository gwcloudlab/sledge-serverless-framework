reset

set term jpeg size 1024,768
set output "success_part1.jpg"

set xlabel "Replenishment Period (ms)"
set ylabel "Deadline success rate %"

set xrange [0:]
set yrange [0:105]

set style histogram columnstacked
set key horizontal

plot 'success_part1.dat' using 1:2 title 'RU=5%' linetype 1 linecolor 0 with linespoints, \
     'success_part1.dat' using 1:3 title 'RU=10%' lt 1 lc 1 w lp, \
     'success_part1.dat' using 1:4 title 'RU=15%' lt 1 lc 2 w lp, \
     'success_part1.dat' using 1:5 title 'RU=20%' lt 1 lc 3 w lp, \
     'success_part1.dat' using 1:6 title 'RU=25%' lt 1 lc 4 w lp, \
	 'success_part1.dat' using 1:7 title 'RU=30%' lt 1 lc 5 w lp, \
	 'success_part1.dat' using 1:8 title 'RU=35%' lt 1 lc 6 w lp, \
	 'success_part1.dat' using 1:9 title 'RU=40%' lt 1 lc 7 w lp, \
	 'success_part1.dat' using 1:10 title 'RU=45%' lt 1 lc 8 w lp, \
     'success_part1.dat' using 1:11 title 'RU=50%' lt 1 lc 9 w lp
