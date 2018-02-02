# -*- mode: ruby -*-
# vi: set ft=ruby :

Vagrant.configure("2") do |config|
  config.vm.box = if ENV.has_key?("SHILL_GUI")
                  then "shill/freebsd-9.3-mate"
                  else "shill/freebsd-9.3"
                  end

  config.vm.network "private_network", type: "dhcp"
  config.vm.synced_folder ".", "/vagrant", disabled: true
  config.vm.synced_folder ".", "/home/vagrant/shill", type: "nfs"
  config.vm.synced_folder "../ShillBSD", "/usr/src", type: "nfs"

  config.vm.provider "virtualbox" do |vb|
    vb.gui = ENV.has_key?("SHILL_GUI")
    vb.memory = "4096"
  end

  config.vm.provision "shell", inline: <<-SHELL
    pkg install -y racket git
  SHELL

  config.vm.provision "shell", privileged: false, inline: <<-SHELL
    cd /home/vagrant/shill/racket && sudo raco link -i -n shill .
    cd /home/vagrant/shill/stdlib && sudo raco link -i -n shill .
    cd /home/vagrant/shill && make && sudo make install
    sudo sh -c 'echo shill_load="YES" >> /boot/loader.conf'
  SHELL

  config.vm.provision :reload
end
