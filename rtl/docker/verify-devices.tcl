package require ::quartus::device

set parts [get_part_list -family {MAX II}]
foreach required {EPM240T100C5 EPM240T100I5 EPM570T100C5} {
    if {[lsearch -exact $parts $required] < 0} {
        error "MAX II support is missing required part $required"
    }
}

puts "Verified MAX II parts: EPM240T100C5 EPM240T100I5 EPM570T100C5"
