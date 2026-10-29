#!/usr/bin/perl
# Drives the serial console of the Alpine guest that `rhv docker up` starts (QEMU serial on tcp:127.0.0.1:PORT).
#
#   serial-drive.pl PORT login              wait for the login prompt and log in as root
#   serial-drive.pl PORT run SECONDS CMD... run the commands one by one, print their output
#
# perl is part of macOS itself, so nothing has to be installed on the user's machine.
use strict;
use warnings;
use IO::Socket::INET;
use IO::Select;

my ($port, $mode, @args) = @ARGV;
my $sock;
for (1 .. 60) {
    $sock = IO::Socket::INET->new(PeerAddr => "127.0.0.1:$port", Proto => 'tcp') and last;
    sleep 1;
}
die "cannot connect to the guest console on port $port\n" unless $sock;
$sock->autoflush(1);
my $sel = IO::Select->new($sock);
my $buf = '';
my $log;
open($log, '>>', $ENV{RHV_SERIAL_LOG}) if $ENV{RHV_SERIAL_LOG};

sub expect {
    my ($re, $timeout) = @_;
    my $end = time + $timeout;
    while (time < $end) {
        if ($buf =~ s/\A(.*?$re)//s) { return $1; }
        if ($sel->can_read(0.5)) {
            my $n = sysread($sock, my $chunk, 65536);
            die "the guest console closed\n" unless $n;
            print $log $chunk if $log;
            $buf .= $chunk;
        }
    }
    my $tail = substr($buf, -300);
    die "timeout waiting for /$re/; last output: $tail\n";
}

if ($mode eq 'login') {
    print $sock "\n";
    expect(qr/login: /, 900);
    print $sock "root\n";
    expect(qr/# /, 60);
    print "logged in\n";
} elsif ($mode eq 'run') {
    my $limit = shift @args;
    for my $cmd (@args) {
        print $sock "$cmd; echo __RC_\$?__\n";
        my $out = expect(qr/__RC_(\d+)__\r?\n/, $limit);
        $out =~ s/\A.*?\n//s if $out =~ /echo __RC_/;     # drop the echoed command line
        print $out;
    }
} else {
    die "usage: $0 PORT login | PORT run SECONDS CMD...\n";
}
